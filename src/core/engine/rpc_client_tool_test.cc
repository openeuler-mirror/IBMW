// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for RpcClientTool::AbandonAll: every pending record gets
// exactly one result, and timeouts firing afterwards are no-ops.

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
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

}  // namespace
}  // namespace ibmw::runtime::core::util
