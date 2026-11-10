// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for RpcClientTool::AbandonAll: every pending record gets
// exactly one result, and timeouts firing afterwards are no-ops. A record
// whose timeout cannot be scheduled is not kept (TRANS-049). The tests from
// "Integration tests" on run against real executors. All of them run against
// whichever hash map backend is configured (selfdev ConcurrentHashMap or tbb).

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cpp/util/string.h"
#include "engine/executor/executor_access.h"
#include "engine/executor/parallel_executor.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/rpc_client_tool.h"

namespace ibmw::runtime::core::util {
namespace {

using namespace std::chrono_literals;

// Timed executor whose tasks only run when the test says so.
class ManualTimedExecutor {
 public:
  ibmw::executor::ExecutorHandle Handle() { return ibmw::executor::ExecutorHandle(&base_); }
  void RunOne() {
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    task();
  }
  void RunAll() {
    while (!tasks_.empty()) {
      auto task = std::move(tasks_.front());
      tasks_.pop_front();
      task();
    }
  }

 private:
  static ManualTimedExecutor* Self(void* ctx) { return static_cast<ManualTimedExecutor*>(ctx); }

  std::deque<ibmw::executor::Task> tasks_;
  ibmw_executor_base_t base_{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("manual"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("manual"); },
      .is_thread_safe = [](void*) { return false; },
      .is_in_current_executor = [](void*) { return false; },
      .supports_timed_dispatch = [](void*) { return true; },
      .dispatch = [](void* ctx, ibmw_function_base_t* task) { Self(ctx)->tasks_.emplace_back(task); },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns = [](void* ctx, uint64_t, ibmw_function_base_t* task) { Self(ctx)->tasks_.emplace_back(task); },
      .ctx = this};
};

// TRANS-004: an answer that does not belong to the call leaves it pending.
TEST(RpcClientToolTest, GetRecordIfKeepsNonMatchingRecord) {
  RpcClientTool<int> tool;
  ASSERT_TRUE(tool.Record(7, 1s, 10));
  EXPECT_FALSE(tool.GetRecordIf(7, [](int record) { return record == 11; }));
  EXPECT_FALSE(tool.GetRecordIf(8, [](int) { return true; }));
  auto record = tool.GetRecordIf(7, [](int record) { return record == 10; });
  ASSERT_TRUE(record);
  EXPECT_EQ(*record, 10);
  EXPECT_FALSE(tool.GetRecord(7));
  EXPECT_NE(RandomReqIdStart(), RandomReqIdStart());  // 2^-32 to fail by chance
}

TEST(RpcClientToolTest, AbandonAllCompletesEachPendingRecordOnce) {
  RpcClientTool<int> tool;
  ASSERT_TRUE(tool.Record(1, 1s, 10));
  ASSERT_TRUE(tool.Record(2, 1s, 20));
  ASSERT_TRUE(tool.Record(3, 1s, 30));
  ASSERT_EQ(tool.GetRecord(2), 20);  // answered

  std::vector<int> abandoned;
  tool.AbandonAll([&](int&& v) { abandoned.push_back(v); });
  std::sort(abandoned.begin(), abandoned.end());
  EXPECT_EQ(abandoned, (std::vector<int>{10, 30}));
  EXPECT_FALSE(tool.GetRecord(1));
  EXPECT_FALSE(tool.GetRecord(3));
}

TEST(RpcClientToolTest, TimeoutsAfterAbandonAllAreNoops) {
  ManualTimedExecutor executor;
  RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(executor.Handle());
  int timeouts = 0;
  tool.RegisterTimeoutHandle([&](int&&) { ++timeouts; });

  ASSERT_TRUE(tool.Record(1, 1s, 10));
  int abandoned = 0;
  tool.AbandonAll([&](int&&) { ++abandoned; });
  executor.RunAll();
  EXPECT_EQ(abandoned, 1);
  EXPECT_EQ(timeouts, 0);
}

TEST(RpcClientToolTest, TimeoutCompletesPendingRecord) {
  ManualTimedExecutor executor;
  RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(executor.Handle());
  std::vector<int> timed_out;
  tool.RegisterTimeoutHandle([&](int&& v) { timed_out.push_back(v); });

  ASSERT_TRUE(tool.Record(1, 1s, 10));
  ASSERT_TRUE(tool.Record(2, 1s, 20));
  ASSERT_EQ(tool.GetRecord(2), 20);
  executor.RunAll();
  EXPECT_EQ(timed_out, (std::vector<int>{10}));
  int abandoned = 0;
  tool.AbandonAll([&](int&&) { ++abandoned; });
  EXPECT_EQ(abandoned, 0);
}

// A timeout handler still running on another thread is waited for: each
// record still gets exactly one result (timeout or abandon).
// COMM-013: after a wrap a new call skips the ids of calls still pending.
TEST(RpcClientToolTest, RetryRecordUsesNextFreeId) {
  RpcClientTool<int> tool;
  ASSERT_TRUE(tool.Record(7, 1s, 70));
  ASSERT_TRUE(tool.Record(8, 1s, 80));

  uint32_t counter = 7;
  uint32_t id = 7;
  EXPECT_FALSE(tool.Record(id, 1s, 90));
  EXPECT_TRUE(tool.RetryRecord(id, 1s, 90, [&] { return ++counter; }));
  EXPECT_EQ(id, 9u);
  EXPECT_EQ(tool.GetRecord(7), 70);
  EXPECT_EQ(tool.GetRecord(8), 80);
  EXPECT_EQ(tool.GetRecord(9), 90);

  // Bounded: gives up when every id tried is taken.
  ASSERT_TRUE(tool.Record(100, 1s, 1));
  id = 100;
  int attempts = 0;
  EXPECT_FALSE(tool.RetryRecord(id, 1s, 2, [&] {
    ++attempts;
    return 100u;
  }));
  EXPECT_EQ(attempts, 8);
  EXPECT_EQ(tool.GetRecord(100), 1);
}

TEST(RpcClientToolTest, AbandonAllWaitsForRunningTimeoutTask) {
  ManualTimedExecutor executor;
  RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(executor.Handle());
  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  std::vector<int> timed_out;
  tool.RegisterTimeoutHandle([&](int&& v) {
    timed_out.push_back(v);
    entered.set_value();
    released.wait();
  });
  ASSERT_TRUE(tool.Record(1, 1s, 10));
  ASSERT_TRUE(tool.Record(2, 1s, 20));

  std::thread timeout_thread([&] { executor.RunOne(); });  // times out record 1
  entered.get_future().wait();

  std::vector<int> abandoned;
  std::atomic<bool> done{false};
  std::thread abandon_thread([&] {
    tool.AbandonAll([&](int&& v) { abandoned.push_back(v); });
    done = true;
  });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(done.load()) << "AbandonAll did not wait for the running timeout task";
  release.set_value();
  timeout_thread.join();
  abandon_thread.join();
  executor.RunAll();  // record 2's timeout: a no-op now

  EXPECT_EQ(timed_out, (std::vector<int>{10}));
  EXPECT_EQ(abandoned, (std::vector<int>{20}));
}

TEST(RpcClientToolTest, AbandonAllContainsHandlerExceptions) {
  RpcClientTool<int> tool;
  for (uint32_t i = 1; i <= 4; ++i) ASSERT_TRUE(tool.Record(i, 1s, static_cast<int>(i)));
  int calls = 0;
  tool.AbandonAll([&](int&& v) {
    ++calls;
    if (v % 2) throw std::runtime_error("boom");
  });
  EXPECT_EQ(calls, 4);
  for (uint32_t i = 1; i <= 4; ++i) EXPECT_FALSE(tool.GetRecord(i));
}

// A record whose timeout cannot be scheduled would never end: it is not kept.
TEST(RpcClientToolTest, RecordIsRolledBackWhenTheTimeoutCannotBeScheduled) {
  static const ibmw_executor_base_t kThrowing{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("throwing"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("throwing"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor = [](void*) { return false; },
      .supports_timed_dispatch = [](void*) { return true; },
      .dispatch = [](void*, ibmw_function_base_t* task) { ibmw::executor::Task dropped(task); },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns =
          [](void*, uint64_t, ibmw_function_base_t* task) {
            ibmw::executor::Task dropped(task);
            throw std::bad_alloc();
          },
      .ctx = nullptr};
  RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle(&kThrowing));
  EXPECT_FALSE(tool.Record(1, 1s, 10));
  EXPECT_FALSE(tool.GetRecord(1));
}

// Integration tests.

using ibmw::runtime::core::executor::ExecutorAccess;
using ibmw::runtime::core::executor::ParallelExecutor;
using ibmw::runtime::core::executor::ThreadPoolExecutor;

// Move-only, like the recorders the rpc backends store.
struct TestRecorder {
  uint32_t req_id = 0;
  std::unique_ptr<int> payload;
};

TestRecorder MakeRecorder(uint32_t req_id) {
  return TestRecorder{req_id, std::make_unique<int>(static_cast<int>(req_id) * 3)};
}

TEST(RpcClientToolTest, RecordThenGetRecordReturnsOnce) {
  RpcClientTool<TestRecorder> tool;
  ASSERT_TRUE(tool.Record(11, 1s, MakeRecorder(11)));

  auto first = tool.GetRecord(11);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->req_id, 11u);
  ASSERT_TRUE(first->payload);
  EXPECT_EQ(*first->payload, 33);

  EXPECT_FALSE(tool.GetRecord(11).has_value());
  EXPECT_FALSE(tool.GetRecord(12).has_value());
}

TEST(RpcClientToolTest, DuplicateReqIdRecordReturnsFalse) {
  RpcClientTool<TestRecorder> tool;
  ASSERT_TRUE(tool.Record(5, 1s, TestRecorder{5, std::make_unique<int>(1)}));
  EXPECT_FALSE(tool.Record(5, 1s, TestRecorder{5, std::make_unique<int>(2)}));

  // The first record is kept.
  auto rec = tool.GetRecord(5);
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->payload);
  EXPECT_EQ(*rec->payload, 1);

  // Once taken, the id can be recorded again.
  EXPECT_TRUE(tool.Record(5, 1s, TestRecorder{5, std::make_unique<int>(3)}));
  rec = tool.GetRecord(5);
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(*rec->payload, 3);
}

TEST(RpcClientToolTest, RegisterNonTimedExecutorThrows) {
  ParallelExecutor exe;
  exe.Initialize("rpc_parallel", YAML::Load("{thread_num: 1}"));
  exe.Start();
  ExecutorAccess access(&exe);

  RpcClientTool<TestRecorder> tool;
  EXPECT_THROW(tool.RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle(access.NativeHandle())),
               std::runtime_error);

  exe.Shutdown();
}

TEST(RpcClientToolTest, TimeoutDeliversUnansweredRecord) {
  ThreadPoolExecutor exe;
  exe.Initialize("rpc_timeout", YAML::Load("{thread_num: 1}"));
  exe.Start();
  ExecutorAccess access(&exe);

  std::atomic_int timeouts = 0;
  std::atomic_uint32_t timed_out_id = 0;
  {
    RpcClientTool<TestRecorder> tool;
    tool.RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle(access.NativeHandle()));
    tool.RegisterTimeoutHandle([&](TestRecorder&& rec) {
      timed_out_id = rec.req_id;
      ++timeouts;
    });

    ASSERT_TRUE(tool.Record(21, 10ms, MakeRecorder(21)));
    ASSERT_TRUE(tool.Record(22, 10ms, MakeRecorder(22)));
    // 22 is answered in time; only 21 times out.
    EXPECT_TRUE(tool.GetRecord(22).has_value());

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (timeouts.load() == 0 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
    EXPECT_EQ(timeouts.load(), 1);
    EXPECT_EQ(timed_out_id.load(), 21u);
    EXPECT_FALSE(tool.GetRecord(21).has_value());

    // Shutdown waits for the remaining timer of 22 while the tool is alive.
    exe.Shutdown();
  }
  EXPECT_EQ(timeouts.load(), 1);
}

TEST(RpcClientToolTest, TimeoutRacingResponseDeliversExactlyOnce) {
  constexpr uint32_t kReqs = 4000;
  constexpr int kResponders = 4;

  ThreadPoolExecutor exe;
  exe.Initialize("rpc_race", YAML::Load("{thread_num: 2}"));
  exe.Start();
  ExecutorAccess access(&exe);

  std::vector<std::atomic_int> delivered(kReqs);
  std::atomic_int timeout_count = 0;
  std::atomic_int response_count = 0;
  {
    RpcClientTool<TestRecorder> tool;
    tool.RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle(access.NativeHandle()));
    tool.RegisterTimeoutHandle([&](TestRecorder&& rec) {
      ++delivered[rec.req_id];
      ++timeout_count;
    });

    // Timeouts of 0..1ms land while the responders take the same ids.
    std::atomic_uint32_t recorded = 0;
    std::thread recorder([&]() {
      for (uint32_t id = 0; id < kReqs; ++id) {
        EXPECT_TRUE(tool.Record(id, std::chrono::microseconds(id % 1000), MakeRecorder(id)));
        recorded.store(id + 1, std::memory_order_release);
      }
    });

    std::vector<std::thread> responders;
    for (int r = 0; r < kResponders; ++r) {
      responders.emplace_back([&, r]() {
        for (uint32_t id = static_cast<uint32_t>(r); id < kReqs; id += kResponders) {
          while (recorded.load(std::memory_order_acquire) <= id) std::this_thread::yield();
          if (id % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(id % 500));
          auto rec = tool.GetRecord(id);
          if (rec) {
            EXPECT_EQ(rec->req_id, id);
            ++delivered[id];
            ++response_count;
          }
        }
      });
    }
    recorder.join();
    for (auto& t : responders) t.join();

    // Let every pending timer fire while the tool is still alive.
    exe.Shutdown();
  }

  int not_once = 0;
  for (uint32_t id = 0; id < kReqs; ++id) {
    if (delivered[id].load() != 1) ++not_once;
  }
  EXPECT_EQ(not_once, 0);
  EXPECT_EQ(timeout_count.load() + response_count.load(), static_cast<int>(kReqs));
}

TEST(RpcClientToolTest, ConcurrentRecordSameIdHasSingleWinner) {
  constexpr int kThreads = 8;
  constexpr uint32_t kRounds = 500;
  RpcClientTool<TestRecorder> tool;

  for (uint32_t round = 0; round < kRounds; ++round) {
    std::atomic_int record_wins = 0;
    std::atomic_int take_wins = 0;
    std::atomic_int ready = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        ++ready;
        while (ready.load() < kThreads) std::this_thread::yield();
        if (tool.Record(round, 1s, MakeRecorder(round))) ++record_wins;
      });
    }
    for (auto& t : threads) t.join();
    threads.clear();
    ready = 0;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&]() {
        ++ready;
        while (ready.load() < kThreads) std::this_thread::yield();
        if (tool.GetRecord(round)) ++take_wins;
      });
    }
    for (auto& t : threads) t.join();

    ASSERT_EQ(record_wins.load(), 1) << "round " << round;
    ASSERT_EQ(take_wins.load(), 1) << "round " << round;
  }
}

}  // namespace
}  // namespace ibmw::runtime::core::util
