// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-049: without a configured timeout_executor the DDS RPC client uses a
// built-in timer, so a call a native .srv server never answers still ends.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#include "dds_ext/rpc_timeout_timer.h"
#include "engine/rpc_client_tool.h"

namespace ibmw::extensions::dds_extension {
namespace {

using namespace std::chrono_literals;

TEST(RpcTimeoutTimerTest, RunsTasksInDeadlineOrder) {
  RpcTimeoutTimer timer;
  auto handle = timer.Handle();
  ASSERT_TRUE(handle.SupportsTimedDispatch());
  std::mutex mutex;
  std::vector<int> order;
  std::promise<void> done;
  const auto add = [&](int id, std::chrono::milliseconds after) {
    handle.DispatchAfter(after, [&, id] {
      std::lock_guard lock(mutex);
      order.push_back(id);
      if (order.size() == 3) done.set_value();
    });
  };
  add(3, 120ms);
  add(1, 20ms);  // earlier than the one the thread sleeps for
  add(2, 60ms);
  ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(RpcTimeoutTimerTest, RunsOnItsOwnThread) {
  RpcTimeoutTimer timer;
  auto handle = timer.Handle();
  std::promise<bool> inside;
  handle.Dispatch([&] { inside.set_value(handle.IsInCurrentExecutor()); });
  auto result = inside.get_future();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(result.get());
  EXPECT_FALSE(handle.IsInCurrentExecutor());
}

TEST(RpcTimeoutTimerTest, StopDropsPendingTasks) {
  auto ran = std::make_shared<std::atomic<bool>>(false);
  auto alive = std::make_shared<int>(0);
  RpcTimeoutTimer timer;
  timer.Handle().DispatchAfter(1h, [ran, alive] { *ran = true; });
  timer.Stop();
  EXPECT_EQ(alive.use_count(), 1);  // the task is destroyed
  timer.Handle().Dispatch([ran, alive] { *ran = true; });
  EXPECT_EQ(alive.use_count(), 1);
  EXPECT_FALSE(*ran);
}

// A recorded call nobody answers gets the timeout result.
TEST(RpcTimeoutTimerTest, UnansweredClientCallTimesOut) {
  RpcTimeoutTimer timer;
  runtime::core::util::RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(timer.Handle());
  std::promise<int> timed_out;
  tool.RegisterTimeoutHandle([&](int&& call) { timed_out.set_value(call); });
  ASSERT_TRUE(tool.Record(1, 30ms, 41));
  ASSERT_TRUE(tool.Record(2, 30ms, 42));
  EXPECT_EQ(tool.GetRecord(2), 42);  // answered in time
  auto result = timed_out.get_future();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(result.get(), 41);
  EXPECT_FALSE(tool.GetRecord(1).has_value());
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
