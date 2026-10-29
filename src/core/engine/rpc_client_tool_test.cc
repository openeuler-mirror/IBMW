// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for RpcClientTool::AbandonAll: every pending record gets
// exactly one result, and timeouts firing afterwards are no-ops.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cpp/util/string.h"
#include "engine/rpc_client_tool.h"

namespace ibmw::runtime::core::util {
namespace {

using namespace std::chrono_literals;

// Timed executor whose tasks only run when the test says so.
class ManualTimedExecutor {
 public:
  executor::ExecutorHandle Handle() { return executor::ExecutorHandle(&base_); }
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

  std::deque<executor::Task> tasks_;
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

}  // namespace
}  // namespace ibmw::runtime::core::util
