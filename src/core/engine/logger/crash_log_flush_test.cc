// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// The crash report is logged through the asynchronous log drivers and then
// the process ends: FlushForCrash() waits (bounded) until the report is
// written, and never waits on the log executor's own thread.

#include <gtest/gtest.h>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "engine/executor/watchdog_executor.h"
#include "engine/logger/console_logger_driver.h"
#include "engine/logger/logging_subsystem.h"
#include "engine/logger/rotate_file_logger_driver.h"

namespace ibmw::runtime::core::logger {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::string MakeTempDir() {
  std::string tmpl = ::testing::TempDir() + "ibmw_crash_log_flush.XXXXXX";
  const char* dir = mkdtemp(tmpl.data());
  EXPECT_NE(dir, nullptr);
  return dir == nullptr ? std::string() : std::string(dir);
}

std::string ReadFile(const std::string& path) {
  std::ifstream ifs(path);
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

LogDataWrapper MakeLogData(const char* text) {
  return LogDataWrapper{
      .module_name = "core",
      .thread_id = 0,
      .t = std::chrono::system_clock::now(),
      .lvl = IBMW_LOG_LEVEL_ERROR,
      .line = 1,
      .file_name = "crash_log_flush_test.cc",
      .function_name = "MakeLogData",
      .log_data = text,
      .log_data_size = std::strlen(text)};
}

// A watchdog executor (the log executor of both drivers) with a gate task
// that keeps it busy until released, so later log tasks stay queued.
class LogExecutorFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    exe_.Initialize(YAML::Load("{name: crash_flush_test}"));
    exe_.Start();
  }

  void TearDown() override {
    Release();
    exe_.Shutdown();
  }

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> GetExecutorFunc() {
    return [this](std::string_view) { return ibmw::executor::ExecutorHandle(exe_.NativeHandle()); };
  }

  // Blocks the log executor until Release(), or for `hold` when given.
  void Block(std::chrono::milliseconds hold = std::chrono::milliseconds::max()) {
    auto started = std::make_shared<std::promise<void>>();
    auto started_future = started->get_future();
    exe_.Dispatch([this, started, hold]() {
      started->set_value();
      const auto until = hold == std::chrono::milliseconds::max() ? Clock::time_point::max() : Clock::now() + hold;
      while (!released_.load() && Clock::now() < until) std::this_thread::sleep_for(1ms);
      gate_done_.store(true);
    });
    started_future.wait();
  }

  void Release() { released_.store(true); }

  executor::WatchdogExecutor exe_;
  std::atomic_bool released_{false};
  std::atomic_bool gate_done_{false};
};

// CH-47
TEST_F(LogExecutorFixture, RotateFileFlushForCrashWritesQueuedReport) {
  const std::string dir = MakeTempDir();
  RotateFileLoggerDriver driver;
  driver.RegisterGetExecutorFunc(GetExecutorFunc());
  driver.Initialize(YAML::Load("{path: '" + dir + "', filename: crash.log, suffix_with_timestamp: false}"));
  driver.Start();

  Block(100ms);
  driver.Log(MakeLogData("report line *** end of stacktrace"));
  EXPECT_EQ(ReadFile(dir + "/crash.log").find("*** end of stacktrace"), std::string::npos);

  const auto t0 = Clock::now();
  EXPECT_TRUE(driver.FlushForCrash(Clock::now() + 2s));
  EXPECT_LT(Clock::now() - t0, 2s);
  EXPECT_TRUE(gate_done_.load());
  // Written and flushed out of the ofstream buffer while the driver is
  // still open (as when the process ends right after the report).
  EXPECT_NE(ReadFile(dir + "/crash.log").find("report line *** end of stacktrace"), std::string::npos);
  driver.Shutdown();
}

// CH-47
TEST_F(LogExecutorFixture, ConsoleFlushForCrashRunsAfterQueuedReport) {
  ConsoleLoggerDriver driver;
  driver.RegisterGetExecutorFunc(GetExecutorFunc());
  driver.Initialize(YAML::Load("{print_color: false}"));
  driver.Start();

  ::testing::internal::CaptureStdout();
  Block(100ms);
  driver.Log(MakeLogData("console report *** end of stacktrace"));
  const bool flushed = driver.FlushForCrash(Clock::now() + 2s);
  const bool gate_done = gate_done_.load();
  const std::string out = ::testing::internal::GetCapturedStdout();
  EXPECT_TRUE(flushed);
  EXPECT_TRUE(gate_done);
  EXPECT_NE(out.find("console report *** end of stacktrace"), std::string::npos) << out;
  driver.Shutdown();
}

// A crash on the log executor thread itself: the queued tasks cannot run
// before the handler returns, so FlushForCrash returns false at once.
// CH-47
TEST_F(LogExecutorFixture, FlushForCrashOnLogExecutorThreadReturnsAtOnce) {
  const std::string dir = MakeTempDir();
  RotateFileLoggerDriver rotate;
  rotate.RegisterGetExecutorFunc(GetExecutorFunc());
  rotate.Initialize(YAML::Load("{path: '" + dir + "', filename: crash.log, suffix_with_timestamp: false}"));
  ConsoleLoggerDriver console;
  console.RegisterGetExecutorFunc(GetExecutorFunc());
  console.Initialize(YAML::Load("{print_color: false}"));

  std::promise<std::pair<bool, Clock::duration>> rotate_result;
  std::promise<std::pair<bool, Clock::duration>> console_result;
  exe_.Dispatch([&]() {
    auto t0 = Clock::now();
    const bool r = rotate.FlushForCrash(Clock::now() + 2s);
    rotate_result.set_value({r, Clock::now() - t0});
    t0 = Clock::now();
    const bool c = console.FlushForCrash(Clock::now() + 2s);
    console_result.set_value({c, Clock::now() - t0});
  });
  auto rotate_future = rotate_result.get_future();
  auto console_future = console_result.get_future();
  ASSERT_EQ(rotate_future.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(console_future.wait_for(5s), std::future_status::ready);
  const auto [rotate_ok, rotate_took] = rotate_future.get();
  const auto [console_ok, console_took] = console_future.get();
  EXPECT_FALSE(rotate_ok);
  EXPECT_FALSE(console_ok);
  EXPECT_LT(rotate_took, 100ms);
  EXPECT_LT(console_took, 100ms);
  rotate.Shutdown();
  console.Shutdown();
}

// A stuck log executor: FlushForCrash gives up at the deadline.
// CH-47
TEST_F(LogExecutorFixture, FlushForCrashIsBoundedWhenLogExecutorIsStuck) {
  ConsoleLoggerDriver driver;
  driver.RegisterGetExecutorFunc(GetExecutorFunc());
  driver.Initialize(YAML::Load("{print_color: false}"));
  driver.Start();

  Block();
  const auto t0 = Clock::now();
  EXPECT_FALSE(driver.FlushForCrash(Clock::now() + 300ms));
  const auto took = Clock::now() - t0;
  EXPECT_GE(took, 300ms);
  EXPECT_LT(took, 2s);
  Release();
  driver.Shutdown();
}

// ---- the crash handler path (death tests) ---------------------------------

volatile int* volatile g_crash_null = nullptr;

// Not instrumented: under ASan/UBSan the null store would be reported
// instead of raising a real SIGSEGV.
__attribute__((noinline, no_sanitize("address", "undefined"))) void CrashNow() {
  *g_crash_null = 42;
}

// What the engine sets up for the crash log: watchdog executor, logging
// subsystem with enable_crash_log and a rotate_file backend, core logger
// routed to the drivers (Engine::SetCoreLogger). Lives until the crash.
struct CrashLogSetup {
  explicit CrashLogSetup(const std::string& dir) {
    exe.Initialize(YAML::Load("{name: crash_flush_test}"));
    exe.Start();
    logger = std::make_shared<ibmw::common::util::LoggerWrapper>();
    logging.SetLogger(logger);
    logging.RegisterGetExecutorFunc(
        [this](std::string_view) { return ibmw::executor::ExecutorHandle(exe.NativeHandle()); });
    logging.Initialize(YAML::Load(
        "{enable_crash_log: true, backends: [{type: rotate_file, options: {path: '" + dir +
        "', filename: crash.log, suffix_with_timestamp: false}}]}"));
    const auto* core = logging.GetLoggerAccess("core").NativeHandle();
    logger->get_log_level_func = [core]() -> uint32_t { return core->get_log_level(core->ctx); };
    logger->log_func = [core](uint32_t lvl, uint32_t line, const char* file_name, const char* function_name,
                              const char* log_data, size_t log_data_size) {
      core->log(core->ctx, static_cast<ibmw_log_level_t>(lvl), line, file_name, function_name, log_data,
                log_data_size);
    };
    logging.Start();
  }

  executor::WatchdogExecutor exe;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger;
  LoggingSubsystem logging;
};

// The report line is complete when the formatter's "(file:line)" suffix,
// written after the whole message, is in the log.
bool LogHasCompleteReport(const std::string& log) {
  const size_t begin = log.find("Crash signal handling");
  return begin != std::string::npos && log.find("(crash_signal_handling.cc:", begin) != std::string::npos;
}

class CrashLogFlushDeathTest : public ::testing::Test {
 protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }

  // The "threadsafe" death test re-runs the test body in a new process, so a
  // directory created there would differ from the one the parent reads: the
  // parent creates a new one for every test and passes it on in the
  // environment, the death test child uses that one.
  static std::string LogDir() {
    static constexpr const char* kEnv = "IBMW_CRASH_LOG_FLUSH_TEST_DIR";
    if (!GTEST_FLAG_GET(internal_run_death_test).empty()) {
      const char* dir = getenv(kEnv);
      return dir == nullptr ? std::string() : std::string(dir);
    }
    const std::string dir = MakeTempDir();
    EXPECT_EQ(setenv(kEnv, dir.c_str(), 1), 0);
    return dir;
  }
};

// The log executor is still busy with earlier work when the crash happens,
// so the report's write task is queued behind it: without the flush the
// process ends before the report reaches the file.
// CH-48
TEST_F(CrashLogFlushDeathTest, ReportReachesRotateFileBeforeProcessEnds) {
  const std::string dir = LogDir();
  EXPECT_EXIT(
      {
        auto* setup = new CrashLogSetup(dir);
        setup->exe.Dispatch([]() { std::this_thread::sleep_for(150ms); });
        CrashNow();
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  const std::string log = ReadFile(dir + "/crash.log");
  EXPECT_TRUE(LogHasCompleteReport(log)) << log;
}

// A crash on the log executor thread: no wait on the executor (it is the
// crashing thread), the process still ends with its signal, quickly.
// CH-48
TEST_F(CrashLogFlushDeathTest, CrashOnLogExecutorThreadDoesNotHang) {
  const std::string dir = LogDir();
  const auto t0 = Clock::now();
  EXPECT_EXIT(
      {
        auto* setup = new CrashLogSetup(dir);
        setup->exe.Dispatch([]() { CrashNow(); });
        for (;;) std::this_thread::sleep_for(1s);
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  // Well below the selfdev handler's 10 s report watchdog: the flush did not
  // wait on its own thread (backward-cpp has no watchdog and would hang).
  EXPECT_LT(Clock::now() - t0, 5s);
}

// A stuck log executor: the flush gives up after kCrashFlushTimeout and the
// process ends with its signal.
// CH-48
TEST_F(CrashLogFlushDeathTest, StuckLogExecutorDelaysCrashOnlyByFlushTimeout) {
  const std::string dir = LogDir();
  const auto t0 = Clock::now();
  EXPECT_EXIT(
      {
        auto* setup = new CrashLogSetup(dir);
        setup->exe.Dispatch([]() {
          for (;;) std::this_thread::sleep_for(1s);
        });
        std::this_thread::sleep_for(20ms);
        CrashNow();
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  EXPECT_LT(Clock::now() - t0, 5s);
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
