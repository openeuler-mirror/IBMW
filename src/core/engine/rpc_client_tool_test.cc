// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// RpcClientTool integration tests. They run against whichever hash map
// backend is configured (selfdev ConcurrentHashMap or tbb).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/executor/executor_access.h"
#include "engine/executor/parallel_executor.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/rpc_client_tool.h"

namespace ibmw::runtime::core::util {
namespace {

using namespace std::chrono_literals;
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
