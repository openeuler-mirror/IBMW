// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// End-to-end executor benchmark comparing the selfdev backends with the
// third-party ones (oneTBB / libunifex) through the real IBMW executors:
//   1. ParallelExecutor Dispatch throughput, 4 workers, 1/4/8 producers.
//   2. co_await Schedule(exe) hops between two executors: ParallelExecutors
//      (queue + coroutine) and ThreadPoolExecutors (asio, identical in both
//      builds, so only the coroutine layer differs).
//   3. RpcClientTool Record + GetRecord throughput from 4 threads.
// Each metric prints one "RESULT <name> <value> <unit>" line.
// Built with -DIBMW_BUILD_SELFDEV_E2E_BENCH=ON; not registered with ctest.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "base/ibmw_config.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "cpp/co/task.h"
#include "engine/executor/executor_access.h"
#include "engine/executor/parallel_executor.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/rpc_client_tool.h"

namespace ibmw::runtime::core::executor {
namespace {

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void Report(const std::string& name, double value, const char* unit) {
  std::printf("RESULT %s %.4f %s\n", name.c_str(), value, unit);
  std::fflush(stdout);
}

void WaitUntil(const std::atomic_uint64_t& counter, uint64_t target) {
  while (counter.load(std::memory_order_acquire) < target) std::this_thread::yield();
}

// Initialized and started executor plus its C ABI handle.
template <typename Executor>
class BenchExecutor {
 public:
  BenchExecutor(std::string_view name, uint32_t threads) : access_(&executor_) {
    // threshold_alarm_interval_ms < 0 disables the ParallelExecutor queue
    // threshold check; ThreadPoolExecutor ignores the key.
    executor_.Initialize(name, YAML::Load("{thread_num: " + std::to_string(threads) +
                                          ", threshold_alarm_interval_ms: -1}"));
    executor_.Start();
  }
  ~BenchExecutor() { executor_.Shutdown(); }

  BenchExecutor(const BenchExecutor&) = delete;
  BenchExecutor& operator=(const BenchExecutor&) = delete;

  Executor& Get() { return executor_; }
  co::IbmwExecutor Co() const {
    return co::IbmwExecutor(ibmw::executor::ExecutorHandle(access_.NativeHandle()));
  }

 private:
  Executor executor_;
  ExecutorAccess access_;
};

void BenchDispatch(uint32_t producers, uint64_t total) {
  BenchExecutor<ParallelExecutor> exe("bench_parallel", 4);
  std::atomic_uint64_t executed = 0;
  const uint64_t per_producer = total / producers;
  const uint64_t expected = per_producer * producers;

  std::atomic_bool go = false;
  std::vector<std::thread> threads;
  for (uint32_t ii = 0; ii < producers; ++ii) {
    threads.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      for (uint64_t jj = 0; jj < per_producer; ++jj)
        exe.Get().Dispatch([&executed]() { executed.fetch_add(1, std::memory_order_release); });
    });
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : threads) t.join();
  WaitUntil(executed, expected);
  const double sec = SecondsSince(start);
  Report("dispatch_p" + std::to_string(producers) + "_w4", expected / sec / 1e6, "Mtask/s");
}

co::Task<void> PingPong(co::IbmwExecutor a, co::IbmwExecutor b, uint64_t round_trips) {
  for (uint64_t ii = 0; ii < round_trips; ++ii) {
    co_await co::Schedule(a);
    co_await co::Schedule(b);
  }
}

template <typename Executor>
void BenchCoHops(const char* exe_tag, uint64_t round_trips, uint32_t chains, uint32_t threads_per_exe) {
  BenchExecutor<Executor> exe_a("bench_co_a", threads_per_exe);
  BenchExecutor<Executor> exe_b("bench_co_b", threads_per_exe);
  const uint64_t per_chain = round_trips / chains;

  std::atomic_bool go = false;
  std::vector<std::thread> waiters;
  for (uint32_t ii = 0; ii < chains; ++ii) {
    waiters.emplace_back([&]() {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      co::SyncWait(PingPong(exe_a.Co(), exe_b.Co(), per_chain));
    });
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : waiters) t.join();
  const double sec = SecondsSince(start);
  const double hops = 2.0 * static_cast<double>(per_chain * chains);

  const std::string tag = std::string("co_hop_") + exe_tag + "_c" + std::to_string(chains) + "_t" +
                          std::to_string(threads_per_exe);
  Report(tag + "_throughput", hops / sec / 1e6, "Mhop/s");
  if (chains == 1) Report(tag + "_roundtrip_latency", sec / static_cast<double>(per_chain) * 1e9, "ns");
}

struct BenchRecorder {
  uint64_t seq = 0;
  uint64_t stamp = 0;
  uint64_t cookie[2] = {0, 0};
};

void BenchRpcTool(uint32_t threads, uint32_t per_thread, uint32_t window) {
  util::RpcClientTool<BenchRecorder> tool;
  std::atomic_uint64_t found = 0;
  std::atomic_bool go = false;
  std::vector<std::thread> workers;
  for (uint32_t tt = 0; tt < threads; ++tt) {
    workers.emplace_back([&, tt]() {
      const uint32_t base = tt * per_thread;
      uint64_t local_found = 0;
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      // Keep `window` requests in flight, answering the oldest one.
      for (uint32_t ii = 0; ii < per_thread + window; ++ii) {
        if (ii < per_thread) tool.Record(base + ii, std::chrono::seconds(1), BenchRecorder{ii, ii, {ii, tt}});
        if (ii >= window && tool.GetRecord(base + ii - window)) ++local_found;
      }
      found += local_found;
    });
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : workers) t.join();
  const double sec = SecondsSince(start);
  const uint64_t total = static_cast<uint64_t>(threads) * per_thread;
  if (found.load() != total) {
    std::fprintf(stderr, "rpc tool lost records: %lu of %lu\n",
                 static_cast<unsigned long>(found.load()), static_cast<unsigned long>(total));
    std::exit(1);
  }
  Report("rpc_tool_t" + std::to_string(threads) + "_w" + std::to_string(window), total / sec / 1e6, "Mpair/s");
}

}  // namespace
}  // namespace ibmw::runtime::core::executor

int main(int argc, char** argv) {
  using namespace ibmw::runtime::core::executor;

  // Usage: ibmw_selfdev_e2e_bench [scale] [dispatch|co|rpc]
  // scale shrinks the iteration counts (e.g. 0.1 for a smoke run); the
  // second argument runs only one group (all groups by default).
  const double scale = (argc > 1) ? std::atof(argv[1]) : 1.0;
  const std::string group = (argc > 2) ? argv[2] : "";
  auto n = [scale](double v) { return static_cast<uint64_t>(v * scale) + 1; };
  auto enabled = [&group](const char* name) { return group.empty() || group == name; };

#if defined(IBMW_SELFDEV_CONCURRENT)
  std::printf("CONFIG concurrent=selfdev\n");
#else
  std::printf("CONFIG concurrent=tbb\n");
#endif
#if defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)
  std::printf("CONFIG coro=selfdev\n");
#elif defined(IBMW_EXECUTOR_USE_STDEXEC)
  std::printf("CONFIG coro=stdexec\n");
#else
  std::printf("CONFIG coro=unifex\n");
#endif

  if (enabled("dispatch")) {
    for (uint32_t producers : {1u, 4u, 8u}) BenchDispatch(producers, n(4e6));
  }
  if (enabled("co")) {
    BenchCoHops<ParallelExecutor>("parallel", n(2e5), 1, 1);
    BenchCoHops<ParallelExecutor>("parallel", n(1e6), 8, 4);
    BenchCoHops<ThreadPoolExecutor>("asio", n(2e5), 1, 1);
    BenchCoHops<ThreadPoolExecutor>("asio", n(1e6), 8, 4);
  }
  if (enabled("rpc")) {
    BenchRpcTool(4, static_cast<uint32_t>(n(1e6)), 1);
    BenchRpcTool(4, static_cast<uint32_t>(n(1e6)), 1024);
  }
  return 0;
}
