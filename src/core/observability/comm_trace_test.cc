#include "observability/comm_trace.h"
#include "engine/transport/internal/comm_trace_send_overlay.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace ibmw::runtime::core::observability::comm_trace {
namespace {

std::atomic<uint64_t> clock_calls{0};

uint64_t CountingClock() noexcept {
  return 1000 + clock_calls.fetch_add(1, std::memory_order_relaxed);
}

class CommTraceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_for_test();
    clock_calls.store(0, std::memory_order_relaxed);
    SetClockForTest(&CountingClock);
  }
};

TEST_F(CommTraceTest, OffEmitsNothingAndDoesNotReadClock) {
  SetModeForTest(RuntimeMode::off);
  Emit(EventKind::publish_enter);
  {
    Scope scope(EventKind::callback_enter, EventKind::callback_exit);
  }

  Event event{};
  EXPECT_EQ(drain(&event, 1), 0U);
  EXPECT_EQ(clock_calls.load(std::memory_order_relaxed), 0U);
}

TEST_F(CommTraceTest, OffCaptureReturnsInvalidWithoutReadingClock) {
  SetModeForTest(RuntimeMode::off);
  const auto envelope = CapturePublishEnvelope();
  EXPECT_FALSE(envelope.valid());
  EXPECT_EQ(clock_calls.load(std::memory_order_relaxed), 0U);
}

TEST_F(CommTraceTest, FullAndSampledCaptureImmediately) {
  SetModeForTest(RuntimeMode::full);
  const auto full = CapturePublishEnvelope();
  ASSERT_TRUE(full.valid());
  EXPECT_EQ(full.publish_enter_ns, 1000U);

  SetModeForTest(RuntimeMode::sampled);
  const auto sampled = CapturePublishEnvelope();
  ASSERT_TRUE(sampled.valid());
  EXPECT_EQ(sampled.publish_enter_ns, 1001U);
  EXPECT_EQ(clock_calls.load(std::memory_order_relaxed), 2U);
}

TEST_F(CommTraceTest, FullScopeDrainsEnterAndExit) {
  SetModeForTest(RuntimeMode::full);
  {
    Scope scope(EventKind::publish_enter, EventKind::publish_return, 42, 7, 3);
  }

  Event events[2]{};
  ASSERT_EQ(drain(events, 2), 2U);
  EXPECT_EQ(events[0].kind, EventKind::publish_enter);
  EXPECT_EQ(events[1].kind, EventKind::publish_return);
  EXPECT_EQ(events[0].token, 42U);
  EXPECT_EQ(events[0].timestamp_ns, 1000U);
  EXPECT_EQ(events[1].timestamp_ns, 1001U);
}

TEST_F(CommTraceTest, CallbackRegistrationScopeRestoresNestedIdentity) {
  EXPECT_EQ(CurrentCallbackRegistrationId(), 0U);
  {
    CallbackScope outer(17);
    EXPECT_EQ(CurrentCallbackRegistrationId(), 17U);
    {
      CallbackScope inner(29);
      EXPECT_EQ(CurrentCallbackRegistrationId(), 29U);
    }
    EXPECT_EQ(CurrentCallbackRegistrationId(), 17U);
  }
  EXPECT_EQ(CurrentCallbackRegistrationId(), 0U);
}

TEST_F(CommTraceTest, CallbackEventUsesCurrentRegistrationIdentity) {
  SetModeForTest(RuntimeMode::full);
  {
    CallbackScope callback_registration(73);
    IBMW_COMM_TRACE_CALLBACK_SCOPE();
  }

  Event events[2]{};
  ASSERT_EQ(drain(events, 2), 2U);
  EXPECT_EQ(events[0].kind, EventKind::callback_enter);
  EXPECT_EQ(events[1].kind, EventKind::callback_exit);
  EXPECT_EQ(events[0].token, 73U);
  EXPECT_EQ(events[1].token, 73U);
}

TEST_F(CommTraceTest, FullBufferDropsNewEventsWithoutOverwrite) {
  SetModeForTest(RuntimeMode::full);
  for (uint64_t token = 1; token <= kCapacity / 2; ++token) {
    Emit(EventKind::publish_enter, token);
  }
  Emit(EventKind::publish_enter, 9999);

  std::vector<Event> events(kCapacity);
  ASSERT_EQ(drain(events.data(), events.size()), kCapacity);
  EXPECT_EQ(dropped(), 1U);
  for (size_t i = 0; i < events.size(); i += 2) {
    EXPECT_EQ(events[i].token, i / 2 + 1);
    EXPECT_EQ(events[i].token, events[i + 1].token);
  }
}

TEST_F(CommTraceTest, FullBufferDropsWholeSpanWithoutOrphan) {
  SetModeForTest(RuntimeMode::full);
  for (uint64_t token = 1; token <= kCapacity / 2; ++token) {
    Scope scope(EventKind::publish_enter, EventKind::publish_return, token);
  }
  {
    Scope scope(EventKind::publish_enter, EventKind::publish_return, 9999);
  }

  std::vector<Event> events(kCapacity);
  ASSERT_EQ(drain(events.data(), events.size()), kCapacity);
  EXPECT_EQ(dropped(), 1U);
  for (size_t i = 0; i < events.size(); i += 2) {
    EXPECT_EQ(events[i].kind, EventKind::publish_enter);
    EXPECT_EQ(events[i + 1].kind, EventKind::publish_return);
    EXPECT_EQ(events[i].token, events[i + 1].token);
  }
}

TEST_F(CommTraceTest, SampledModeIsDeterministicAndZeroTokenIsEligible) {
  SetModeForTest(RuntimeMode::sampled);
  for (uint64_t token = 1; token <= 32; ++token) {
    Emit(EventKind::publish_enter, token);
  }
  for (size_t i = 0; i < 8; ++i) Emit(EventKind::publish_enter, 0);

  Event first_run[80]{};
  const size_t first_count = drain(first_run, 80);
  ASSERT_GT(first_count, 0U);
  size_t sampled_zero_tokens = 0;
  for (size_t i = 0; i < first_count; i += 2) {
    if (first_run[i].token == 0) ++sampled_zero_tokens;
  }
  EXPECT_GT(sampled_zero_tokens, 0U);
  EXPECT_LT(sampled_zero_tokens, 8U);

  reset_for_test();
  SetClockForTest(&CountingClock);
  SetModeForTest(RuntimeMode::sampled);
  for (uint64_t token = 1; token <= 32; ++token) {
    Emit(EventKind::publish_enter, token);
  }
  for (size_t i = 0; i < 8; ++i) Emit(EventKind::publish_enter, 0);
  Event second_run[80]{};
  const size_t second_count = drain(second_run, 80);
  ASSERT_EQ(second_count, first_count);
  for (size_t i = 0; i < first_count; ++i) {
    EXPECT_EQ(second_run[i].token, first_run[i].token);
  }
}

TEST_F(CommTraceTest, PendingSnapshotIsBoundedDuringConcurrentDrain) {
  SetModeForTest(RuntimeMode::full);
  constexpr size_t kThreads = 4;
  constexpr size_t kEventsPerThread = 32;
  constexpr size_t kTotalEvents = kThreads * kEventsPerThread * 2;
  Event events[kTotalEvents]{};
  std::atomic_bool producers_done{false};
  size_t drained = 0;
  std::thread consumer([&] {
    while (!producers_done.load(std::memory_order_acquire) ||
           drained < kTotalEvents) {
      EXPECT_LE(pending(), kThreads * kCapacity);
      drained += drain(events + drained, kTotalEvents - drained);
      if (drained < kTotalEvents) std::this_thread::yield();
    }
  });

  std::vector<std::thread> threads;
  for (size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([thread] {
      for (size_t i = 0; i < kEventsPerThread; ++i) {
        Emit(EventKind::callback_enter, thread * kEventsPerThread + i + 1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  producers_done.store(true, std::memory_order_release);
  consumer.join();

  EXPECT_EQ(drained, kTotalEvents);
  EXPECT_EQ(dropped(), 0U);
}

TEST_F(CommTraceTest, ThreadCapacityAndDropAccountingAreIsolated) {
  SetModeForTest(RuntimeMode::full);
  std::atomic<uint64_t> first_thread_drops{0};
  std::atomic<uint64_t> second_thread_drops{0};
  std::thread first([&] {
    for (size_t i = 0; i < kCapacity / 2 + 3; ++i) {
      Emit(EventKind::callback_enter, 1000 + i);
    }
    first_thread_drops.store(current_thread_dropped(),
                             std::memory_order_release);
  });
  std::thread second([&] {
    for (size_t i = 0; i < 4; ++i) {
      Emit(EventKind::callback_enter, 2000 + i);
    }
    second_thread_drops.store(current_thread_dropped(),
                              std::memory_order_release);
  });
  first.join();
  second.join();

  std::vector<Event> events(kCapacity + 8);
  ASSERT_EQ(drain(events.data(), events.size()), kCapacity + 8);
  EXPECT_EQ(first_thread_drops.load(std::memory_order_acquire), 3U);
  EXPECT_EQ(second_thread_drops.load(std::memory_order_acquire), 0U);
  EXPECT_EQ(dropped(), 3U);
  EXPECT_EQ(registry_dropped(), 0U);
  EXPECT_EQ(registered_threads(), 0U);
  size_t second_thread_events = 0;
  for (const Event& event : events) {
    if (event.token >= 2000 && event.token < 2004) ++second_thread_events;
  }
  EXPECT_EQ(second_thread_events, 8U);
}

TEST_F(CommTraceTest, RegistryLimitDropsWithoutDanglingThreadState) {
  SetModeForTest(RuntimeMode::full);
  for (size_t i = 0; i < kMaxThreadRings + 1; ++i) {
    std::thread producer([i] {
      Emit(EventKind::callback_enter, i + 1);
    });
    producer.join();
  }

  EXPECT_EQ(registered_threads(), kMaxThreadRings);
  EXPECT_EQ(pending(), kMaxThreadRings * 2);
  EXPECT_EQ(registry_dropped(), 1U);
  EXPECT_EQ(dropped(), 1U);
}

void ReturnThroughScope() {
  IBMW_COMM_TRACE_SCOPE(publish_enter, publish_return);
  return;
}

TEST_F(CommTraceTest, ScopePairsAnEarlyReturn) {
  SetModeForTest(RuntimeMode::full);
  ReturnThroughScope();
  Event events[2]{};
  ASSERT_EQ(drain(events, 2), 2U);
  EXPECT_EQ(events[0].kind, EventKind::publish_enter);
  EXPECT_EQ(events[1].kind, EventKind::publish_return);
}

TEST_F(CommTraceTest, DrainCapacityOneNeverSplitsSpan) {
  SetModeForTest(RuntimeMode::full);
  { Scope scope(EventKind::publish_enter, EventKind::publish_return, 7); }
  Event events[2]{};
  EXPECT_EQ(drain(events, 1), 0U);
  EXPECT_EQ(pending(), 2U);
  EXPECT_EQ(drain(events, 2), 2U);
  EXPECT_EQ(events[0].token, events[1].token);
}

TEST_F(CommTraceTest, InvalidSpanKindsAreDroppedBeforeEnteringRing) {
  SetModeForTest(RuntimeMode::full);
  {
    Scope scope(EventKind::publish_enter, EventKind::callback_exit, 8);
  }
  Event events[2]{};
  EXPECT_EQ(drain(events, 2), 0U);
  EXPECT_EQ(pending(), 0U);
  EXPECT_EQ(dropped(), 1U);
}

TEST_F(CommTraceTest, EmptyThreadRingIsReleasedAtThreadExit) {
  SetModeForTest(RuntimeMode::full);
  std::thread producer([] {
    { Scope scope(EventKind::callback_enter, EventKind::callback_exit, 1); }
    Event events[2]{};
    EXPECT_EQ(drain(events, 2), 2U);
  });
  producer.join();
  EXPECT_EQ(registered_threads(), 0U);
}

TEST_F(CommTraceTest, RetiredRingIsReleasedAfterDrain) {
  SetModeForTest(RuntimeMode::full);
  std::thread([] {
    Scope scope(EventKind::callback_enter, EventKind::callback_exit, 9);
  }).join();
  EXPECT_EQ(registered_threads(), 1U);
  EXPECT_EQ(pending(), 2U);
  Event events[2]{};
  ASSERT_EQ(drain(events, 2), 2U);
  EXPECT_EQ(registered_threads(), 0U);
}

TEST_F(CommTraceTest, ConcurrentDrainsNeverDuplicateSpans) {
  SetModeForTest(RuntimeMode::full);
  constexpr size_t kSpans = 64;
  for (size_t i = 0; i < kSpans; ++i) {
    Scope scope(EventKind::publish_enter, EventKind::publish_return, i + 1);
  }
  Event first[kCapacity]{};
  Event second[kCapacity]{};
  std::atomic_bool start{false};
  size_t first_count = 0;
  size_t second_count = 0;
  std::thread a([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    first_count = drain(first, kCapacity);
  });
  std::thread b([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    second_count = drain(second, kCapacity);
  });
  start.store(true, std::memory_order_release);
  a.join();
  b.join();
  EXPECT_EQ(first_count + second_count, kSpans * 2);
  EXPECT_EQ(first_count % 2, 0U);
  EXPECT_EQ(second_count % 2, 0U);
  EXPECT_EQ(pending(), 0U);
}

TEST_F(CommTraceTest, ThreadChurnReusesSlotsBeyondRegistryCapacity) {
  SetModeForTest(RuntimeMode::full);
  constexpr size_t kThreads = kMaxThreadRings * 4;
  for (size_t i = 0; i < kThreads; ++i) {
    std::thread([i] {
      Scope scope(EventKind::callback_enter, EventKind::callback_exit, i + 1);
    }).join();
    Event events[2]{};
    ASSERT_EQ(drain(events, 2), 2U);
  }
  EXPECT_EQ(registry_dropped(), 0U);
  EXPECT_EQ(registered_threads(), 0U);
}

TEST_F(CommTraceTest, CapabilityIsExplicitlyStageOnly) {
  static_assert(kStageOnly);
  EXPECT_TRUE(kStageOnly);
}

}  // namespace
}  // namespace ibmw::runtime::core::observability::comm_trace
