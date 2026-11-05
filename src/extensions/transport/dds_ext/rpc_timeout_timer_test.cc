// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-049: without a configured timeout_executor the DDS RPC client uses a
// built-in timer, so a call a native .srv server never answers still ends.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
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

// A timeout handle that shuts the driver down hands out the other pending
// calls without waiting for its own task.
TEST(RpcTimeoutTimerTest, AbandonAllFromTheTimeoutHandle) {
  RpcTimeoutTimer timer;
  runtime::core::util::RpcClientTool<int> tool;
  tool.RegisterTimeoutExecutor(timer.Handle());
  std::vector<int> abandoned;
  std::promise<void> done;
  tool.RegisterTimeoutHandle([&](int&&) {
    tool.AbandonAll([&](int&& call) { abandoned.push_back(call); });
    done.set_value();
  });
  ASSERT_TRUE(tool.Record(1, 20ms, 41));
  ASSERT_TRUE(tool.Record(2, 1h, 42));
  ASSERT_EQ(done.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(abandoned, (std::vector<int>{42}));
}

// A timeout callback may shut the driver down: Stop from a task does not
// join its own thread, and the thread still ends.
TEST(RpcTimeoutTimerTest, StopFromATaskDoesNotJoinItself) {
  auto timer = std::make_unique<RpcTimeoutTimer>();
  std::promise<void> stopped;
  auto alive = std::make_shared<int>(0);
  std::weak_ptr<int> watch = alive;
  timer->Handle().Dispatch([&, alive = std::move(alive)] {
    timer->Stop();
    stopped.set_value();
  });
  ASSERT_EQ(stopped.get_future().wait_for(5s), std::future_status::ready);
  for (int i = 0; i < 500 && !watch.expired(); ++i) std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(watch.expired());  // the task ended and was destroyed
  timer.reset();
}

// After a Stop from a task, a Stop from another thread still waits for the
// task, and the thread id is not kept once the thread is gone.
TEST(RpcTimeoutTimerTest, StopAfterSelfStopWaitsForTheTask) {
  RpcTimeoutTimer timer;
  auto handle = timer.Handle();
  std::promise<void> self_stopped;
  std::atomic<bool> finished{false};
  handle.Dispatch([&] {
    timer.Stop();
    self_stopped.set_value();
    std::this_thread::sleep_for(100ms);
    finished = true;
  });
  ASSERT_EQ(self_stopped.get_future().wait_for(5s), std::future_status::ready);
  timer.Stop();
  EXPECT_TRUE(finished);
  EXPECT_FALSE(handle.IsInCurrentExecutor());
}

// An "infinite" deadline does not wrap around to an immediate one.
TEST(RpcTimeoutTimerTest, HugeDeadlineIsNotRunEarly) {
  RpcTimeoutTimer timer;
  auto ran = std::make_shared<std::atomic<bool>>(false);
  const auto* table = timer.Handle().NativeHandle();
  ibmw::executor::Task never([ran] { *ran = true; });
  table->dispatch_at_ns(table->ctx, UINT64_MAX, never.NativeHandle());  // beyond int64 nanoseconds
  timer.Handle().DispatchAfter(std::chrono::nanoseconds::max() / 2, [ran] { *ran = true; });
  std::promise<void> later;
  timer.Handle().DispatchAfter(50ms, [&] { later.set_value(); });
  ASSERT_EQ(later.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(*ran);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
