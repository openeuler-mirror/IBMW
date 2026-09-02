// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for the iceoryx zero-copy POD path:
//   1. URL routing helper (POD vs standard prefix do not collide)
//   2. IoxLoanedSample<T> RAII semantics (move, release-on-destroy)
//   3. IoxStylePublisher<T> static_assert constraints (compile-only checks)
//
// Plus tests for the iceoryx zero-copy ctx-meta path (stage 6.1.d):
//   4. ComputeCtxMetaHeaderSize correctness against Send()'s wire layout
//   5. EncodeCtxMetaHeader byte-level round-trip via ConstBufferOperator
//   6. EncodeCtxMetaHeader rejects oversized inputs / undersized buffers
//   7. IoxCtxMetaLoanedBuffer RAII semantics
//
// These tests do NOT require a running RouDi daemon. End-to-end cross-process
// validation lives in the transport_benchmark sample (stage 6.2).

#include <gtest/gtest.h>

#include <cstring>
#include <future>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "base/buffer_util.h"
#include "engine/transport/loaned_sample_handle.h"
#include "iceoryx_ext/iox_style_publisher.h"
#include "iceoryx_ext/iceoryx_bus.h"

namespace ibmw::extensions::iceoryx_extension {

TEST(IceoryxShutdown, OrdinaryTestThreadIsNotAListenerCallback) {
  EXPECT_FALSE(IsIceoryxListenerCallbackThread());
}

TEST(SubscriberOptions, LegacyUsesDefaultsAndRetainedIsBounded) {
  const auto legacy = MakeSubscriberOptions(false);
  EXPECT_EQ(legacy.queueCapacity, 1u);
  EXPECT_EQ(legacy.queueFullPolicy,
            iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA);
  EXPECT_EQ(legacy.historyRequest, 0u);

  const auto retained = MakeSubscriberOptions(true);
  EXPECT_EQ(retained.queueCapacity, 1u);
  EXPECT_EQ(retained.queueFullPolicy,
            iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA);
  EXPECT_EQ(retained.historyRequest, 0u);

  const auto lossless = MakeSubscriberOptions(
      false, IoxDeliveryOptions{.lossless = true, .queue_capacity = 256});
  EXPECT_EQ(lossless.queueCapacity, 256u);
  EXPECT_EQ(lossless.queueFullPolicy, iox::popo::QueueFullPolicy::BLOCK_PRODUCER);
  EXPECT_EQ(lossless.historyRequest, 0u);
}

TEST(DeliveryProfile, RejectsUnknownPartialAndOneSidedControls) {
  IceoryxTransportDriver::Options legacy;
  const auto defaults = IceoryxTransportDriver::ResolveDeliveryOptions(legacy);
  ASSERT_TRUE(defaults);
  EXPECT_FALSE(defaults->lossless);

  IceoryxTransportDriver::Options exact;
  exact.benchmark_profile = "core_metrics_e2e_lossless_v1";
  exact.publisher_too_slow_policy = "WAIT_FOR_CONSUMER";
  exact.subscriber_queue_full_policy = "BLOCK_PRODUCER";
  exact.subscriber_queue_capacity = 256;
  const auto lossless = IceoryxTransportDriver::ResolveDeliveryOptions(exact);
  ASSERT_TRUE(lossless);
  EXPECT_TRUE(lossless->lossless);
  EXPECT_EQ(lossless->queue_capacity, 256u);

  for (const auto& invalid : {
           IceoryxTransportDriver::Options{.benchmark_profile = "unknown"},
           IceoryxTransportDriver::Options{
               .benchmark_profile = "core_metrics_e2e_lossless_v1",
               .publisher_too_slow_policy = "WAIT_FOR_CONSUMER"},
           IceoryxTransportDriver::Options{
               .benchmark_profile = "core_metrics_e2e_lossless_v1",
               .subscriber_queue_capacity = 256,
               .publisher_too_slow_policy = "WAIT_FOR_CONSUMER",
               .subscriber_queue_full_policy = "DISCARD_OLDEST_DATA"}}) {
    EXPECT_FALSE(IceoryxTransportDriver::ResolveDeliveryOptions(invalid));
  }
}

TEST(IoxReleaseToken, DestructorFallbackAndExplicitReleaseAreExactlyOnce) {
  int releases = 0;
  {
    IoxReleaseToken token([&releases] { ++releases; });
  }
  EXPECT_EQ(releases, 1);

  {
    IoxReleaseToken token([&releases] { ++releases; });
    token.Release();
    token.Release();
  }
  EXPECT_EQ(releases, 2);
}

TEST(IoxLeaseLimiter, DropsAtLimitAndAcceptsFutureSampleAfterRelease) {
  IoxLeaseLimiter limiter(1);
  EXPECT_TRUE(limiter.TryAcquire());
  EXPECT_FALSE(limiter.TryAcquire());
  EXPECT_EQ(limiter.Dropped(), 1u);

  limiter.Release();
  EXPECT_TRUE(limiter.TryAcquire());
  EXPECT_EQ(limiter.Outstanding(), 1u);
  EXPECT_EQ(limiter.Dropped(), 1u);
}

TEST(IoxAcquiredPayload,
     ThrowingSetupReleasesPayloadLimiterAndActiveCallbackExactlyOnce) {
  int payload_releases = 0;
  int limiter_releases = 0;
  int active_callback_completions = 0;

  internal::RunIoxAcquiredPayload(
      [&]() noexcept {
        ++payload_releases;
        ++limiter_releases;
      },
      [&]() noexcept { ++active_callback_completions; },
      [](bool&) { throw std::bad_alloc(); });

  EXPECT_EQ(payload_releases, 1);
  EXPECT_EQ(limiter_releases, 1);
  EXPECT_EQ(active_callback_completions, 1);
}

TEST(IoxAcquiredPayload,
     TransferredReleaseTokenPreservesRetainedOwnershipAndCompletesCallback) {
  int releases = 0;
  int active_callback_completions = 0;
  std::shared_ptr<IoxReleaseToken> retained;

  internal::RunIoxAcquiredPayload(
      [&]() noexcept { ++releases; },
      [&]() noexcept { ++active_callback_completions; },
      [&](bool& release_on_exit) {
        retained = std::make_shared<IoxReleaseToken>(
            [&]() noexcept { ++releases; });
        release_on_exit = false;
      });

  EXPECT_EQ(releases, 0);
  EXPECT_EQ(active_callback_completions, 1);
  retained->Release();
  retained.reset();
  EXPECT_EQ(releases, 1);
}

TEST(IoxAcquiredPayload,
     ThrowingAfterTokenTransferReleasesAndCompletesExactlyOnce) {
  int releases = 0;
  int active_callback_completions = 0;

  internal::RunIoxAcquiredPayload(
      [&]() noexcept { ++releases; },
      [&]() noexcept { ++active_callback_completions; },
      [&](bool& release_on_exit) {
        auto token = std::make_shared<IoxReleaseToken>(
            [&]() noexcept { ++releases; });
        release_on_exit = false;
        throw std::bad_alloc();
      });

  EXPECT_EQ(releases, 1);
  EXPECT_EQ(active_callback_completions, 1);
}

TEST(IoxRawLoanRequest, PreservesExactPayloadSizeAndAlignment) {
  const auto request = MakeIoxRawLoanRequest(37, 64);
  ASSERT_TRUE(request);
  EXPECT_EQ(request->payload_size, 37u);
  EXPECT_EQ(request->alignment, 64u);
  EXPECT_FALSE(MakeIoxRawLoanRequest(37, 3));
  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    EXPECT_FALSE(MakeIoxRawLoanRequest(
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1, 8));
  }
}

TEST(IoxAsyncStop, RequestIsNonBlockingRunsActionOnWorkerAndCloseCompletes) {
  std::promise<void> unblock;
  auto gate = unblock.get_future().share();
  std::atomic_int calls{0};
  std::thread::id action_thread;
  const auto request_thread = std::this_thread::get_id();
  IoxAsyncStop stop([&] {
    action_thread = std::this_thread::get_id();
    ++calls;
    gate.wait();
  });
  const auto request_begin = std::chrono::steady_clock::now();
  stop.Request();
  EXPECT_LT(std::chrono::steady_clock::now() - request_begin,
            std::chrono::milliseconds(100));
  stop.Request();

  const auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(stop.Close(std::chrono::milliseconds(20)));
  EXPECT_LT(std::chrono::steady_clock::now() - begin,
            std::chrono::milliseconds(200));
  EXPECT_EQ(calls, 1);
  unblock.set_value();
  EXPECT_TRUE(stop.Close(std::chrono::seconds(1)));
  EXPECT_TRUE(stop.Close(std::chrono::milliseconds(0)));
  EXPECT_NE(action_thread, request_thread);
}

TEST(IoxAsyncStop, CallbackCloseReturnsPromptly) {
  IoxAsyncStop stop([] {});
  stop.Request();
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(stop.Close(std::chrono::seconds(1), true));
  EXPECT_LT(std::chrono::steady_clock::now() - begin,
            std::chrono::milliseconds(100));
  EXPECT_TRUE(stop.Close(std::chrono::seconds(1)));
}

TEST(IoxAsyncStop, DestructorWithoutRequestStopsAndJoinsWorker) {
  const auto begin = std::chrono::steady_clock::now();
  { IoxAsyncStop stop([] { ADD_FAILURE() << "action must not run"; }); }
  EXPECT_LT(std::chrono::steady_clock::now() - begin,
            std::chrono::milliseconds(200));
}

TEST(IoxAsyncStop, ActionCanDestroyOwnerOnWorkerAndFinishSafely) {
  struct StateDestructionProbe {
    explicit StateDestructionProbe(std::promise<void>* destroyed_promise)
        : destroyed_promise(destroyed_promise) {}
    ~StateDestructionProbe() { destroyed_promise->set_value(); }

    std::promise<void>* destroyed_promise;
  };

  std::promise<void> action_started_promise;
  auto action_started = action_started_promise.get_future();
  std::promise<void> allow_destroy_promise;
  auto allow_destroy = allow_destroy_promise.get_future().share();
  std::promise<void> action_finished_promise;
  auto action_finished = action_finished_promise.get_future();
  std::promise<void> state_destroyed_promise;
  auto state_destroyed = state_destroyed_promise.get_future();
  std::unique_ptr<IoxAsyncStop> stop;

  stop = std::make_unique<IoxAsyncStop>(
      [&, probe = std::make_shared<StateDestructionProbe>(
              &state_destroyed_promise)] {
        (void)probe;
        action_started_promise.set_value();
        allow_destroy.wait();
        stop.reset();
        action_finished_promise.set_value();
      });
  stop->Request();
  const auto action_started_status =
      action_started.wait_for(std::chrono::seconds(1));
  EXPECT_EQ(action_started_status, std::future_status::ready);
  allow_destroy_promise.set_value();

  const auto action_finished_status =
      action_finished.wait_for(std::chrono::seconds(1));
  EXPECT_EQ(action_finished_status, std::future_status::ready);
  if (action_finished_status == std::future_status::timeout) {
    action_finished.wait();
  }

  const auto state_destroyed_status =
      state_destroyed.wait_for(std::chrono::seconds(1));
  EXPECT_EQ(state_destroyed_status, std::future_status::ready);
  if (state_destroyed_status == std::future_status::timeout) {
    state_destroyed.wait();
  }
  EXPECT_EQ(stop, nullptr);
}

TEST(IoxSubscriptionRegistry, RejectsDuplicateReservationAndCommit) {
  IoxSubscriptionRegistry registry;
  EXPECT_TRUE(registry.Reserve("/transport/topic/type"));
  EXPECT_FALSE(registry.Reserve("/transport/topic/type"));
  EXPECT_TRUE(registry.Commit("/transport/topic/type", nullptr));
  EXPECT_FALSE(registry.Reserve("/transport/topic/type"));
  EXPECT_FALSE(registry.Commit("/transport/topic/type", nullptr));
}

TEST(IoxSubscriptionRegistry, RollbackAllowsRetry) {
  IoxSubscriptionRegistry registry;
  EXPECT_TRUE(registry.Reserve("/transport/topic/type"));
  registry.Rollback("/transport/topic/type");
  EXPECT_TRUE(registry.Reserve("/transport/topic/type"));
}

TEST(IoxAsyncStop, DetachAndQuiescenceFinishBeforeDeregister) {
  std::promise<void> detach_complete_promise;
  auto detach_complete = detach_complete_promise.get_future().share();
  std::promise<void> callback_quiescent_promise;
  auto callback_quiescent = callback_quiescent_promise.get_future().share();
  std::promise<void> deregistered_promise;
  auto deregistered = deregistered_promise.get_future().share();
  IoxAsyncStop stop([&] {
    detach_complete_promise.set_value();
    callback_quiescent.wait();
    EXPECT_EQ(detach_complete.wait_for(std::chrono::seconds(0)),
              std::future_status::ready);
    deregistered_promise.set_value();
  });

  stop.Request();
  const auto detach_status = detach_complete.wait_for(std::chrono::seconds(1));
  EXPECT_EQ(detach_status, std::future_status::ready);
  EXPECT_EQ(deregistered.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);
  callback_quiescent_promise.set_value();
  EXPECT_TRUE(stop.Close(std::chrono::seconds(1)));
  EXPECT_EQ(deregistered.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
}

TEST(IoxSubscriptionTeardown, FailedWaitsRetainStateUntilEveryGateIsQuiescent) {
  IoxNotificationGate callback_gate;
  ASSERT_TRUE(callback_gate.TryEnter());
  std::promise<void> subscription_quiescent_promise;
  auto subscription_quiescent =
      subscription_quiescent_promise.get_future().share();
  std::promise<void> detached_promise;
  auto detached = detached_promise.get_future();
  std::promise<void> cleaned_promise;
  auto cleaned = cleaned_promise.get_future();
  std::atomic_size_t callback_waits{0};
  std::atomic_size_t subscription_waits{0};

  auto teardown = std::async(std::launch::async, [&] {
    internal::RunIoxSubscriptionTeardown(
        [&] { callback_gate.Close(); },
        [&] { detached_promise.set_value(); },
        [&] {
          ++callback_waits;
          return callback_gate.WaitForQuiescence(std::chrono::milliseconds(5));
        },
        [&] {
          ++subscription_waits;
          return subscription_quiescent.wait_for(std::chrono::milliseconds(5)) ==
                 std::future_status::ready;
        },
        [&] { cleaned_promise.set_value(); });
  });

  ASSERT_EQ(detached.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  while (callback_waits.load() < 2) std::this_thread::yield();
  EXPECT_EQ(cleaned.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  callback_gate.Exit();
  while (subscription_waits.load() < 2) std::this_thread::yield();
  EXPECT_EQ(cleaned.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  subscription_quiescent_promise.set_value();
  EXPECT_EQ(cleaned.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  teardown.get();
}

TEST(IoxNotificationGate, CloseRejectsNewEntrancesAndWaitsForActiveCallback) {
  IoxNotificationGate gate;
  ASSERT_TRUE(gate.TryEnter());
  gate.Close();
  EXPECT_FALSE(gate.TryEnter());
  EXPECT_EQ(gate.Active(), 1u);

  auto wait = std::async(std::launch::async, [&] {
    return gate.WaitForQuiescence(std::chrono::seconds(1));
  });
  EXPECT_EQ(wait.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  gate.Exit();
  EXPECT_TRUE(wait.get());
  EXPECT_EQ(gate.Active(), 0u);
}

TEST(IoxNotificationGate, QuiescenceTimeoutIsBoundedAndFailsClosed) {
  IoxNotificationGate gate;
  ASSERT_TRUE(gate.TryEnter());
  gate.Close();
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.WaitForQuiescence(std::chrono::milliseconds(10)));
  EXPECT_LT(std::chrono::steady_clock::now() - begin,
            std::chrono::milliseconds(200));
  gate.Exit();
  EXPECT_TRUE(gate.WaitForQuiescence(std::chrono::milliseconds(10)));
}

TEST(IoxNotificationGate, SaturatedPackedCountRejectsEntryWithoutOverflow) {
  IoxNotificationGate gate;
  constexpr size_t kSaturatedOpenState =
      std::numeric_limits<size_t>::max() - 1;
  internal::IoxNotificationGateTestAccess::SetPackedState(
      gate, kSaturatedOpenState);

  EXPECT_FALSE(gate.TryEnter());
  EXPECT_EQ(internal::IoxNotificationGateTestAccess::PackedState(gate),
            kSaturatedOpenState);
  EXPECT_FALSE(gate.Closed());

  gate.Close();
  EXPECT_FALSE(gate.TryEnter());
  EXPECT_EQ(internal::IoxNotificationGateTestAccess::PackedState(gate),
            std::numeric_limits<size_t>::max());
  EXPECT_TRUE(gate.Closed());
}

TEST(IoxNotificationGate, WaitReturnAllowsImmediateDestructionAfterLastExit) {
  constexpr size_t kIterations = 1000;
  for (size_t i = 0; i < kIterations; ++i) {
    auto gate = std::make_unique<IoxNotificationGate>();
    ASSERT_TRUE(gate->TryEnter());
    gate->Close();

    auto* const exiting_gate = gate.get();
    std::thread exit([exiting_gate] { exiting_gate->Exit(); });
    ASSERT_TRUE(gate->WaitForQuiescence(std::chrono::seconds(1)));
    gate.reset();
    exit.join();
  }
}

// -----------------------------------------------------------------
// Compile-time checks: POD-only constraint on IoxStylePublisher / Sample.
// -----------------------------------------------------------------
struct PodMessage {
  double x;
  double y;
  uint32_t seq;
};
static_assert(std::is_trivially_copyable_v<PodMessage>);

// IoxLoanedSample<T> and IoxStylePublisher<T> instantiate cleanly for POD.
template class IoxLoanedSample<PodMessage>;
template class IoxStylePublisher<PodMessage>;

// -----------------------------------------------------------------
// LoanMode + LoanedSampleHandle behaviour
// -----------------------------------------------------------------
TEST(LoanedSampleHandle, DefaultIsEmpty) {
  runtime::core::transport::LoanedSampleHandle h;
  EXPECT_EQ(h.user_payload_ptr, nullptr);
  EXPECT_EQ(h.user_payload_size, 0u);
  EXPECT_EQ(h.driver_token, nullptr);
  EXPECT_EQ(h.mode, runtime::core::transport::LoanMode::kPodOnly);
}

// -----------------------------------------------------------------
// IoxLoanedSample lifecycle without driver (driver_=nullptr path).
// -----------------------------------------------------------------
TEST(IoxLoanedSample, EmptyIsInvalidAndNoOpDestroy) {
  IoxLoanedSample<PodMessage> s;
  EXPECT_FALSE(s.is_valid());
  // Destructor must not crash on empty sample.
}

TEST(IoxLoanedSample, MoveTransfersOwnership) {
  // Manually craft a "fake" valid handle pointing into a stack buffer.
  // driver_ is intentionally nullptr so Release() is a no-op (does NOT call
  // driver->ReleaseLoaned), avoiding any need for a live driver.
  alignas(PodMessage) std::byte buf[sizeof(PodMessage)] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = buf;
  handle.user_payload_size = sizeof(PodMessage);
  handle.mode = runtime::core::transport::LoanMode::kPodOnly;
  handle.driver_token = nullptr;

  IoxLoanedSample<PodMessage> a(/*driver=*/nullptr, handle);
  // Without a driver, is_valid() returns false (driver_==nullptr is the
  // canonical "empty/invalid" signal).
  EXPECT_FALSE(a.is_valid());

  // Moving an empty/invalid sample is still well-defined.
  IoxLoanedSample<PodMessage> b = std::move(a);
  EXPECT_FALSE(a.is_valid());
  EXPECT_FALSE(b.is_valid());
}

TEST(IoxLoanedSample, ExtractForPublishClearsState) {
  alignas(PodMessage) std::byte buf[sizeof(PodMessage)] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = buf;
  handle.user_payload_size = sizeof(PodMessage);
  handle.driver_token = reinterpret_cast<void*>(0xDEADBEEF);

  IoxLoanedSample<PodMessage> s(reinterpret_cast<IceoryxTransportDriver*>(0x1),
                                handle);
  EXPECT_TRUE(s.is_valid());

  auto extracted = s.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, buf);
  EXPECT_EQ(extracted.user_payload_size, sizeof(PodMessage));
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xDEADBEEF));

  // After extraction, sample is empty so destructor will not call
  // driver->ReleaseLoaned (we'd otherwise dereference 0x1).
  EXPECT_FALSE(s.is_valid());
}

// =========================================================================
// 6.1.d: ctx-meta header layout tests (no RouDi required).
// =========================================================================
//
// We test ComputeCtxMetaHeaderSize / EncodeCtxMetaHeader against a hand-
// written reference decoder that exactly mirrors the Send()-side parser in
// IceoryxTransportDriver::OnMessage(). If these stay in sync, the on-wire
// bytes produced by the zero-copy ctx-meta path are interchangeable with
// those produced by the regular Send() path -- which is the whole point of
// this mode.

TEST(CtxMetaHeader, EmptyKvSize) {
  std::unordered_map<std::string_view, std::string_view> kv;
  // Layout: pkg_size(4) + 1 + "cdr"(3) + ctx_num(1) = 9
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("cdr", kv), 9u);
}

TEST(CtxMetaHeader, SizeIncludesEachKvWithLengthPrefixes) {
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"k1", "v1"},      // 2+2 + 2+2 = 8
      {"trace", "abc"},  // 2+5 + 2+3 = 12
  };
  // Base = 4 + 1 + 4("json") + 1 = 10; + 8 + 12 = 30
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("json", kv), 30u);
}

TEST(CtxMetaHeader, RejectsOversizedSerializationType) {
  std::string oversize(256, 'x');  // > uint8 max
  std::unordered_map<std::string_view, std::string_view> kv;
  EXPECT_EQ(
      IceoryxTransportDriver::ComputeCtxMetaHeaderSize(oversize, kv), 0u);
}

TEST(CtxMetaHeader, RejectsTooManyKvEntries) {
  // Build 256 entries to overflow the uint8 ctx_num field.
  std::vector<std::string> keys, vals;
  keys.reserve(256);
  vals.reserve(256);
  std::unordered_map<std::string_view, std::string_view> kv;
  kv.reserve(256);
  for (int i = 0; i < 256; ++i) {
    keys.emplace_back("k" + std::to_string(i));
    vals.emplace_back("v" + std::to_string(i));
    kv.emplace(keys.back(), vals.back());
  }
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("cdr", kv), 0u);
}

TEST(CtxMetaHeader, EncodeThenDecodeRoundTrip) {
  // Mirror the parser logic in iceoryx_transport_driver.cc OnMessage().
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"trace_id", "abc123"},
      {"src", "node-A"},
  };
  std::string_view sertype = "cdr";
  size_t header_size =
      IceoryxTransportDriver::ComputeCtxMetaHeaderSize(sertype, kv);
  ASSERT_GT(header_size, 0u);

  // Allocate buffer with extra room for a fake user payload.
  constexpr size_t kPayload = 16;
  std::vector<char> buffer(header_size + kPayload, '\0');
  uint32_t total = static_cast<uint32_t>(buffer.size());

  size_t written = IceoryxTransportDriver::EncodeCtxMetaHeader(
      buffer.data(), buffer.size(), total, sertype, kv);
  ASSERT_EQ(written, header_size);

  // Fill payload region with a marker so we can check the offset is right.
  std::memset(buffer.data() + header_size, 0xAB, kPayload);

  // Decode using the same primitives the receiver uses.
  namespace util = ibmw::common::util;
  uint32_t pkg_size =
      util::GetUint32FromBuf(buffer.data());
  EXPECT_EQ(pkg_size, total);

  util::ConstBufferOperator buf_oper(buffer.data() + 4, pkg_size - 4);
  std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
  EXPECT_EQ(serialization_type, "cdr");

  size_t ctx_num = buf_oper.GetUint8();
  EXPECT_EQ(ctx_num, kv.size());

  std::unordered_map<std::string, std::string> decoded_kv;
  for (size_t ii = 0; ii < ctx_num; ++ii) {
    auto k = buf_oper.GetString(util::BufferLenType::kUInt16);
    auto v = buf_oper.GetString(util::BufferLenType::kUInt16);
    decoded_kv.emplace(std::string(k), std::string(v));
  }
  EXPECT_EQ(decoded_kv.size(), kv.size());
  for (const auto& [k, v] : kv) {
    auto it = decoded_kv.find(std::string(k));
    ASSERT_NE(it, decoded_kv.end()) << "missing key " << k;
    EXPECT_EQ(it->second, std::string(v));
  }

  auto remaining = buf_oper.GetRemainingBuffer();
  ASSERT_EQ(remaining.size(), kPayload);
  for (char b : remaining) {
    EXPECT_EQ(static_cast<unsigned char>(b), 0xAB);
  }
}

TEST(CtxMetaHeader, EncodeFailsOnUndersizedBuffer) {
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"k", "v"},
  };
  std::string_view sertype = "cdr";
  size_t need = IceoryxTransportDriver::ComputeCtxMetaHeaderSize(sertype, kv);
  std::vector<char> small(need - 1, '\0');
  EXPECT_EQ(IceoryxTransportDriver::EncodeCtxMetaHeader(
                small.data(), small.size(),
                static_cast<uint32_t>(need), sertype, kv),
            0u);
}

// IoxCtxMetaLoanedBuffer move-semantics: same shape as IoxLoanedSample tests
// above but without typed access.
TEST(IoxCtxMetaLoanedBuffer, EmptyIsInvalid) {
  IoxCtxMetaLoanedBuffer b;
  EXPECT_FALSE(b.is_valid());
  EXPECT_EQ(b.data(), nullptr);
  EXPECT_EQ(b.size(), 0u);
}

TEST(IoxCtxMetaLoanedBuffer, ExtractForPublishClearsState) {
  std::byte stack_buf[32] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = stack_buf;
  handle.user_payload_size = sizeof(stack_buf);
  handle.mode = runtime::core::transport::LoanMode::kCtxMetaThenPayload;
  handle.driver_token = reinterpret_cast<void*>(0xCAFEBABE);

  IoxCtxMetaLoanedBuffer b(reinterpret_cast<IceoryxTransportDriver*>(0x1),
                           handle);
  EXPECT_TRUE(b.is_valid());
  EXPECT_EQ(b.data(), stack_buf);
  EXPECT_EQ(b.size(), sizeof(stack_buf));

  auto extracted = b.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, stack_buf);
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xCAFEBABE));
  EXPECT_EQ(extracted.mode,
            runtime::core::transport::LoanMode::kCtxMetaThenPayload);
  EXPECT_FALSE(b.is_valid());  // destructor must not deref 0x1
}

// =========================================================================
// 6.2.0: IoxRawPodPublisher (untyped, runtime-sized POD borrow path).
// =========================================================================
//
// These tests verify the *wrapper* surface only -- borrow_loaned_sample()
// against a live driver requires RouDi and is covered by the integration
// sample suite. Here we check:
//   1. The IoxLoanedSample<std::byte> specialisation instantiates cleanly
//      (std::byte is trivially_copyable, so the static_assert holds).
//   2. The raw-publisher header compiles end-to-end (CreateRawPodPublisher
//      + RegisterRawPodSubscriber are well-formed).
//   3. Empty / move semantics for IoxLoanedSample<std::byte> match the
//      typed POD specialisation.

template class IoxLoanedSample<std::byte>;

TEST(IoxRawLoanedSample, EmptyIsInvalid) {
  IoxLoanedSample<std::byte> s;
  EXPECT_FALSE(s.is_valid());
  EXPECT_EQ(s.get(), nullptr);
}

TEST(IoxRawLoanedSample, ExtractForPublishClearsState) {
  std::byte stack_buf[64] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = stack_buf;
  handle.user_payload_size = sizeof(stack_buf);
  handle.mode = runtime::core::transport::LoanMode::kPodOnly;
  handle.driver_token = reinterpret_cast<void*>(0xFEEDFACE);

  IoxLoanedSample<std::byte> s(
      reinterpret_cast<IceoryxTransportDriver*>(0x1), handle);
  EXPECT_TRUE(s.is_valid());
  EXPECT_EQ(s.get(), stack_buf);

  auto extracted = s.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, stack_buf);
  EXPECT_EQ(extracted.user_payload_size, sizeof(stack_buf));
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xFEEDFACE));
  EXPECT_FALSE(s.is_valid());  // destructor must not deref 0x1
}

// Compile-only smoke test: the factory + register helpers are well-formed
// against IceoryxTransportDriver's public surface. We pass a null driver
// so both functions return failure without touching the network/SHM.
TEST(IoxRawPodPublisher, NullDriverFactoryReturnsNullAndSubscriberFails) {
  auto pub = CreateRawPodPublisher(/*driver=*/nullptr,
                                   "/raw/topic", "RawBytes",
                                   /*initial_size_hint=*/4096);
  EXPECT_EQ(pub, nullptr);

  bool ok = RegisterRawPodSubscriber(/*driver=*/nullptr,
                                     "/raw/topic", "RawBytes",
                                     [](const void*, size_t) {});
  EXPECT_FALSE(ok);
}

}  // namespace ibmw::extensions::iceoryx_extension
