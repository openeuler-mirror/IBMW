// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "engine/engine.h"

using namespace std::chrono_literals;
using ibmw::executor::ExecutorHandle;
using ibmw::runtime::core::Engine;

static int g_pass = 0;
static int g_fail = 0;

static std::vector<std::string>& TempConfigFiles() {
  static std::vector<std::string> paths;
  return paths;
}

struct TempConfigCleanup {
  ~TempConfigCleanup() {
    for (const auto& path : TempConfigFiles()) {
      std::remove(path.c_str());
    }
  }
};

#define TEST_ASSERT(cond, msg)                                      \
  do {                                                              \
    if (!(cond)) {                                                  \
      std::cout << "  FAIL: " << (msg) << " [" << __FILE__ << ":" \
                << __LINE__ << "]" << std::endl;                   \
      ++g_fail;                                                     \
      return false;                                                 \
    }                                                               \
  } while (0)

#define TEST_ASSERT_OR_SHUTDOWN(cond, msg, engine, future)          \
  do {                                                              \
    if (!(cond)) {                                                  \
      (engine).Shutdown();                                          \
      (future).wait();                                              \
      std::cout << "  FAIL: " << (msg) << " [" << __FILE__ << ":" \
                << __LINE__ << "]" << std::endl;                   \
      ++g_fail;                                                     \
      return false;                                                 \
    }                                                               \
  } while (0)

#define TEST_CASE(name) \
  std::cout << "--- Test: " << (name) << " ---" << std::endl

#define TEST_PASS(name)                             \
  do {                                              \
    std::cout << "  PASS: " << (name) << std::endl; \
    ++g_pass;                                       \
  } while (0)

static std::string TempDir() {
  if (const char* tmpdir = std::getenv("TMPDIR"); tmpdir != nullptr && *tmpdir != '\0') {
    return tmpdir;
  }
  return "/tmp";
}

static std::string WriteConfig(const std::string& name, const std::string& body) {
  const std::string path = TempDir() + "/ibmw_executor_conformance_" +
                           std::to_string(getpid()) + "_" + name + ".yaml";
  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("failed to open config file " + path);
  }
  f << body;
  if (!f) {
    throw std::runtime_error("failed to write config file " + path);
  }
  f.close();
  TempConfigFiles().push_back(path);
  return path;
}

static bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string FirstExistingFile(const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    if (FileExists(path)) return path;
  }
  return {};
}

static bool WaitUntil(const std::function<bool()>& pred,
                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return pred();
}

static bool WaitCv(std::mutex& mutex, std::condition_variable& cv,
                   const std::function<bool()>& pred,
                   std::chrono::milliseconds timeout = 2s) {
  std::unique_lock<std::mutex> lock(mutex);
  return cv.wait_for(lock, timeout, pred);
}

static std::string PositiveConfig() {
  return R"(ibmw:
  logging:
    core_level: ERROR
  scheduling:
    executors:
      - name: tp1
        type: thread_pool
        options:
          thread_num: 1
          timeout_alarm_interval_ms: -1
      - name: tp4
        type: thread_pool
        options:
          thread_num: 4
          timeout_alarm_interval_ms: -1
      - name: strand_on_tp4
        type: strand
        options:
          bind_thread_pool_executor_name: tp4
          timeout_alarm_interval_ms: -1
      - name: dedicated_fifo
        type: dedicated
      - name: parallel4
        type: parallel
        options:
          thread_num: 4
      - name: timer_standalone
        type: timer
        options:
          dt_us: 2000
          wheel_size: [4, 4]
)";
}

static std::string TimeManipulatorConfig(const std::string& extension_path) {
  std::ostringstream out;
  out << "ibmw:\n"
      << "  extensions:\n"
      << "    items:\n"
      << "      - name: time_manipulator_extension\n"
      << "        path: " << extension_path << "\n"
      << "        options: {}\n"
      << "  logging:\n"
      << "    core_level: ERROR\n"
      << "  scheduling:\n"
      << "    executors:\n"
      << "      - name: tm_bound_pool\n"
      << "        type: thread_pool\n"
      << "        options:\n"
      << "          thread_num: 1\n"
      << "          timeout_alarm_interval_ms: -1\n"
      << "      - name: time_manip\n"
      << "        type: time_manipulator\n"
      << "        options:\n"
      << "          bind_executor: tm_bound_pool\n"
      << "          dt_us: 2000\n"
      << "          init_ratio: 2.0\n"
      << "          wheel_size: [4, 4]\n";
  return out.str();
}

static bool TestPositiveExecutors() {
  TEST_CASE("positive executor conformance");

  try {
    Engine engine;
    Engine::Options options;
    options.cfg_file_path = WriteConfig("positive", PositiveConfig());
    engine.Initialize(options);
    auto fu = engine.AsyncStart();

    auto& bus = engine.GetExecutorBus();
    ExecutorHandle tp1 = bus.GetExecutor("tp1");
    ExecutorHandle tp4 = bus.GetExecutor("tp4");
    ExecutorHandle strand = bus.GetExecutor("strand_on_tp4");
    ExecutorHandle dedicated = bus.GetExecutor("dedicated_fifo");
    ExecutorHandle parallel = bus.GetExecutor("parallel4");
    ExecutorHandle timer = bus.GetExecutor("timer_standalone");

    TEST_ASSERT(tp1 && tp4 && strand && dedicated && parallel && timer,
                "all configured executors should be reachable");

    TEST_ASSERT(tp1.Type() == "thread_pool" && tp1.ThreadSafe(),
                "thread_pool thread_num=1 should be thread-safe");
    TEST_ASSERT(tp1.SupportsTimedDispatch(), "thread_pool should support timed dispatch");
    TEST_ASSERT(!tp4.ThreadSafe(), "thread_pool thread_num=4 should not be thread-safe");

    constexpr int kThreadPoolTasks = 64;
    std::atomic<int> tp_counter{0};
    std::mutex tp_mutex;
    std::condition_variable tp_cv;
    for (int i = 0; i < kThreadPoolTasks; ++i) {
      tp1.Dispatch([&] {
        if (tp_counter.fetch_add(1, std::memory_order_relaxed) + 1 == kThreadPoolTasks) {
          std::lock_guard<std::mutex> lock(tp_mutex);
          tp_cv.notify_one();
        }
      });
    }
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(tp_mutex, tp_cv,
               [&] { return tp_counter.load(std::memory_order_relaxed) == kThreadPoolTasks; }),
        "thread_pool should execute all dispatched tasks", engine, fu);

    std::atomic<bool> tp_timer_fired{false};
    tp1.DispatchAfter(10ms, [&] { tp_timer_fired.store(true, std::memory_order_release); });
    TEST_ASSERT_OR_SHUTDOWN(
        WaitUntil([&] { return tp_timer_fired.load(std::memory_order_acquire); }, 1s),
        "thread_pool DispatchAfter should fire", engine, fu);

    TEST_ASSERT(strand.Type() == "strand" && strand.ThreadSafe(),
                "strand should report thread-safe");
    TEST_ASSERT(!strand.IsInCurrentExecutor(),
                "strand IsInCurrentExecutor is false by design");
    constexpr int kProducerThreads = 4;
    constexpr int kStrandTasksPerProducer = 64;
    constexpr int kStrandTasks = kProducerThreads * kStrandTasksPerProducer;
    int strand_counter = 0;
    int final_strand_counter = -1;
    std::atomic<int> strand_done{0};
    std::atomic<int> active_strand_tasks{0};
    std::atomic<bool> strand_overlap{false};
    std::mutex strand_mutex;
    std::condition_variable strand_cv;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducerThreads; ++p) {
      producers.emplace_back([&] {
        for (int i = 0; i < kStrandTasksPerProducer; ++i) {
          strand.Dispatch([&] {
            if (active_strand_tasks.fetch_add(1, std::memory_order_acq_rel) != 0) {
              strand_overlap.store(true, std::memory_order_release);
            }
            const int next = strand_counter + 1;
            strand_counter = next;
            if (active_strand_tasks.fetch_sub(1, std::memory_order_acq_rel) != 1) {
              strand_overlap.store(true, std::memory_order_release);
            }
            if (strand_done.fetch_add(1, std::memory_order_acq_rel) + 1 == kStrandTasks) {
              std::lock_guard<std::mutex> lock(strand_mutex);
              final_strand_counter = strand_counter;
              strand_cv.notify_one();
            }
          });
        }
      });
    }
    for (auto& producer : producers) producer.join();
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(strand_mutex, strand_cv, [&] { return final_strand_counter >= 0; }),
        "strand should execute all producer tasks", engine, fu);
    TEST_ASSERT(!strand_overlap.load(std::memory_order_acquire),
                "strand should not run callbacks concurrently");
    TEST_ASSERT(final_strand_counter == kStrandTasks,
                "strand should serialize non-atomic increments");

    TEST_ASSERT(dedicated.Type() == "dedicated" && dedicated.ThreadSafe(),
                "dedicated should report thread-safe");
    TEST_ASSERT(!dedicated.SupportsTimedDispatch(),
                "dedicated should not support timed dispatch");
    constexpr int kFifoTasks = 32;
    std::vector<int> order;
    std::mutex fifo_mutex;
    std::condition_variable fifo_cv;
    for (int i = 0; i < kFifoTasks; ++i) {
      dedicated.Dispatch([&, i] {
        std::lock_guard<std::mutex> lock(fifo_mutex);
        order.push_back(i);
        if (static_cast<int>(order.size()) == kFifoTasks) fifo_cv.notify_one();
      });
    }
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(fifo_mutex, fifo_cv,
               [&] { return static_cast<int>(order.size()) == kFifoTasks; }),
        "dedicated should execute FIFO tasks", engine, fu);
    for (int i = 0; i < kFifoTasks; ++i) {
      TEST_ASSERT(order[static_cast<size_t>(i)] == i, "dedicated task order should be FIFO");
    }

    TEST_ASSERT(parallel.Type() == "parallel" && !parallel.ThreadSafe(),
                "parallel thread_num=4 should not be thread-safe");
    TEST_ASSERT(!parallel.SupportsTimedDispatch(),
                "parallel should not support timed dispatch");
    constexpr int kParallelTasks = 128;
    std::atomic<int> parallel_counter{0};
    std::mutex parallel_mutex;
    std::condition_variable parallel_cv;
    for (int i = 0; i < kParallelTasks; ++i) {
      parallel.Dispatch([&] {
        if (parallel_counter.fetch_add(1, std::memory_order_relaxed) + 1 == kParallelTasks) {
          std::lock_guard<std::mutex> lock(parallel_mutex);
          parallel_cv.notify_one();
        }
      });
    }
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(parallel_mutex, parallel_cv,
               [&] { return parallel_counter.load(std::memory_order_relaxed) == kParallelTasks; }),
        "parallel should execute all dispatched tasks", engine, fu);

    TEST_ASSERT(timer.Type() == "timer" && timer.ThreadSafe(),
                "standalone timer should report thread-safe");
    TEST_ASSERT(timer.SupportsTimedDispatch(), "timer should support timed dispatch");
    const auto now1 = timer.Now();
    TEST_ASSERT(WaitUntil([&] { return timer.Now() >= now1; }, 20ms),
                "timer Now should be monotonic");
    const auto base = timer.Now();
    std::vector<int> timer_order;
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    auto record_timer = [&](int token) {
      std::lock_guard<std::mutex> lock(timer_mutex);
      timer_order.push_back(token);
      if (timer_order.size() == 3U) timer_cv.notify_one();
    };
    timer.DispatchAfter(2ms, [&, token = 1] { record_timer(token); });
    timer.DispatchAt(base + 8ms, [&, token = 2] { record_timer(token); });
    timer.DispatchAfter(12ms, [&, token = 3] { record_timer(token); });
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(timer_mutex, timer_cv, [&] { return timer_order.size() == 3U; }),
        "timer DispatchAfter/DispatchAt tasks should fire", engine, fu);
    TEST_ASSERT(timer_order[0] <= timer_order[1] && timer_order[1] <= timer_order[2],
                "timer callbacks should fire in non-decreasing expected order");
    TEST_ASSERT(timer.Now() >= now1, "timer Now should remain monotonic after callbacks");

    engine.Shutdown();
    fu.wait();

    TEST_PASS("positive executor conformance");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

static bool TestTimeManipulatorExecutor() {
  TEST_CASE("time_manipulator extension executor conformance");

  const std::string extension_path = FirstExistingFile({
      "./libibmw_time_manipulator.so",
      "./lib/libibmw_time_manipulator.so",
      "./build/libibmw_time_manipulator.so",
  });
  if (extension_path.empty()) {
    std::cout << "  SKIP: libibmw_time_manipulator.so not available" << std::endl;
    return true;
  }

  try {
    Engine engine;
    Engine::Options options;
    options.cfg_file_path = WriteConfig("time_manipulator", TimeManipulatorConfig(extension_path));
    engine.Initialize(options);
    auto fu = engine.AsyncStart();

    auto& bus = engine.GetExecutorBus();
    ExecutorHandle bound_pool = bus.GetExecutor("tm_bound_pool");
    ExecutorHandle time_manip = bus.GetExecutor("time_manip");

    TEST_ASSERT(bound_pool && time_manip, "time_manipulator executors should be reachable");
    TEST_ASSERT(time_manip.Type() == "time_manipulator", "executor type should be time_manipulator");
    TEST_ASSERT(time_manip.ThreadSafe(), "time_manipulator should mirror bound executor ThreadSafe");
    TEST_ASSERT(time_manip.SupportsTimedDispatch(), "time_manipulator should support timed dispatch");

    std::atomic<bool> dispatch_fired{false};
    std::atomic<bool> ran_on_bound_pool{false};
    std::mutex dispatch_mutex;
    std::condition_variable dispatch_cv;
    time_manip.Dispatch([&] {
      ran_on_bound_pool.store(bound_pool.IsInCurrentExecutor(), std::memory_order_release);
      dispatch_fired.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      dispatch_cv.notify_one();
    });
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(dispatch_mutex, dispatch_cv,
               [&] { return dispatch_fired.load(std::memory_order_acquire); }),
        "time_manipulator Dispatch should delegate to bound executor", engine, fu);
    TEST_ASSERT(ran_on_bound_pool.load(std::memory_order_acquire),
                "time_manipulator callback should run on bound executor");

    const auto now1 = time_manip.Now();
    std::this_thread::sleep_for(15ms);
    const auto now2 = time_manip.Now();
    TEST_ASSERT(now2 > now1, "time_manipulator Now should advance");

    std::atomic<bool> timer_fired{false};
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    time_manip.DispatchAfter(16ms, [&] {
      timer_fired.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(timer_mutex);
      timer_cv.notify_one();
    });
    TEST_ASSERT_OR_SHUTDOWN(
        WaitCv(timer_mutex, timer_cv,
               [&] { return timer_fired.load(std::memory_order_acquire); }),
        "time_manipulator DispatchAfter should fire", engine, fu);

    engine.Shutdown();
    fu.wait();

    TEST_PASS("time_manipulator extension executor conformance");
    return true;
  } catch (const std::exception& e) {
    std::cout << "  FAIL: exception: " << e.what() << std::endl;
    ++g_fail;
    return false;
  }
}

static bool ExpectInitializeFailure(const std::string& name, const std::string& scheduling_yaml) {
  TEST_CASE(name);
  try {
    Engine engine;
    Engine::Options options;
    options.cfg_file_path = WriteConfig(name, "ibmw:\n" + scheduling_yaml);
    engine.Initialize(options);
    std::cout << "  FAIL: Initialize unexpectedly succeeded" << std::endl;
    ++g_fail;
    return false;
  } catch (const std::exception& e) {
    std::cout << "  expected exception: " << e.what() << std::endl;
    TEST_PASS(name);
    return true;
  }
}

static bool ExpectFullConfigInitializeFailure(const std::string& name, const std::string& yaml) {
  TEST_CASE(name);
  try {
    Engine engine;
    Engine::Options options;
    options.cfg_file_path = WriteConfig(name, yaml);
    engine.Initialize(options);
    std::cout << "  FAIL: Initialize unexpectedly succeeded" << std::endl;
    ++g_fail;
    return false;
  } catch (const std::exception& e) {
    std::cout << "  expected exception: " << e.what() << std::endl;
    TEST_PASS(name);
    return true;
  }
}

static std::string TimeManipulatorFailureConfig(
    const std::string& extension_path, const std::string& executor_yaml) {
  std::ostringstream out;
  out << "ibmw:\n"
      << "  extensions:\n"
      << "    items:\n"
      << "      - name: time_manipulator_extension\n"
      << "        path: " << extension_path << "\n"
      << "        options: {}\n"
      << "  logging:\n"
      << "    core_level: ERROR\n"
      << "  scheduling:\n"
      << "    executors:\n"
      << executor_yaml;
  return out.str();
}

static bool TestNegativeCases() {
  bool ok = true;
  ok &= ExpectInitializeFailure("duplicate_executor_name", R"(  scheduling:
    executors:
      - name: dup
        type: thread_pool
      - name: dup
        type: dedicated
)" );
  ok &= ExpectInitializeFailure("unknown_executor_type", R"(  scheduling:
    executors:
      - name: missing
        type: does_not_exist
)" );
  ok &= ExpectInitializeFailure("strand_missing_bind", R"(  scheduling:
    executors:
      - name: strand_bad
        type: strand
)" );
  ok &= ExpectInitializeFailure("strand_bind_missing", R"(  scheduling:
    executors:
      - name: strand_bad
        type: strand
        options:
          bind_thread_pool_executor_name: no_such_pool
)" );
  ok &= ExpectInitializeFailure("strand_bind_not_thread_pool", R"(  scheduling:
    executors:
      - name: dedicated_target
        type: dedicated
      - name: strand_bad
        type: strand
        options:
          bind_thread_pool_executor_name: dedicated_target
)" );
  ok &= ExpectInitializeFailure("parallel_thread_num_zero", R"(  scheduling:
    executors:
      - name: parallel_bad
        type: parallel
        options:
          thread_num: 0
)" );
  ok &= ExpectInitializeFailure("thread_pool_thread_num_zero", R"(  scheduling:
    executors:
      - name: pool_bad
        type: thread_pool
        options:
          thread_num: 0
)" );
  ok &= ExpectInitializeFailure("timer_bind_self", R"(  scheduling:
    executors:
      - name: timer_bad
        type: timer
        options:
          bind_executor: timer_bad
)" );

  const std::string extension_path = FirstExistingFile({
      "./libibmw_time_manipulator.so",
      "./lib/libibmw_time_manipulator.so",
      "./build/libibmw_time_manipulator.so",
  });
  if (!extension_path.empty()) {
    ok &= ExpectFullConfigInitializeFailure(
        "time_manipulator_missing_bind",
        TimeManipulatorFailureConfig(extension_path, R"(      - name: time_manip_bad
        type: time_manipulator
)"));
    ok &= ExpectFullConfigInitializeFailure(
        "time_manipulator_bind_missing",
        TimeManipulatorFailureConfig(extension_path, R"(      - name: time_manip_bad
        type: time_manipulator
        options:
          bind_executor: missing_bound_executor
)"));
  }
  return ok;
}

int main() {
  TempConfigCleanup cleanup;
  std::cout << "IBMW executor conformance standalone test" << std::endl;

  (void)TestPositiveExecutors();
  (void)TestTimeManipulatorExecutor();
  (void)TestNegativeCases();

  std::cout << "\nSummary: pass=" << g_pass << " fail=" << g_fail << std::endl;
  if (g_fail == 0) {
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
  }
  std::cout << "TESTS FAILED" << std::endl;
  return 1;
}
