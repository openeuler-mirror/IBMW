// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// The crash report is logged through the asynchronous log drivers and then
// the process ends: FlushForCrash() waits (bounded) until the report is
// written, and never waits on the log executor's own thread.

#include <gtest/gtest.h>

#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
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

// In milliseconds, so that a failed comparison prints a number.
long long Millis(Clock::duration d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); }

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
// before the handler returns, so FlushForCrash returns false at once. The
// drivers are started, so this is the own-thread check and not the "not
// running" one: without the check the flush task would be queued behind the
// running task and the call would wait until the 2 s deadline.
// CH-47
TEST_F(LogExecutorFixture, FlushForCrashOnLogExecutorThreadReturnsAtOnce) {
  const std::string dir = MakeTempDir();
  RotateFileLoggerDriver rotate;
  rotate.RegisterGetExecutorFunc(GetExecutorFunc());
  rotate.Initialize(YAML::Load("{path: '" + dir + "', filename: crash.log, suffix_with_timestamp: false}"));
  rotate.Start();
  ConsoleLoggerDriver console;
  console.RegisterGetExecutorFunc(GetExecutorFunc());
  console.Initialize(YAML::Load("{print_color: false}"));
  console.Start();

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
  EXPECT_LT(Millis(rotate_took), 100);
  EXPECT_LT(Millis(console_took), 100);
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
  explicit CrashLogSetup(const std::string& dir, bool crash_log = true) {
    exe.Initialize(YAML::Load("{name: crash_flush_test}"));
    exe.Start();
    logger = std::make_shared<ibmw::common::util::LoggerWrapper>();
    logging.SetLogger(logger);
    logging.RegisterGetExecutorFunc(
        [this](std::string_view) { return ibmw::executor::ExecutorHandle(exe.NativeHandle()); });
    logging.Initialize(YAML::Load(
        std::string("{enable_crash_log: ") + (crash_log ? "true" : "false") +
        ", backends: [{type: rotate_file, options: {path: '" + dir +
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

  ~CrashLogSetup() {
    logging.Shutdown();
    exe.Shutdown();
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

// Death test child: records when it is about to crash (steady_clock is
// CLOCK_MONOTONIC, the same in the parent) and turns off core dumps, so the
// parent can measure how long the crash handler took.
void CrashNowTimed(const std::string& dir) {
  const rlimit no_core{0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);
  std::ofstream(dir + "/crash_time") << Clock::now().time_since_epoch().count();
  CrashNow();
}

// Time from CrashNowTimed() in the child until now.
Clock::duration SinceCrash(const std::string& dir) {
  const std::string text = ReadFile(dir + "/crash_time");
  EXPECT_FALSE(text.empty()) << "the child did not reach the crash";
  return Clock::now() - Clock::time_point(Clock::duration(std::stoll(text.empty() ? "0" : text)));
}

// IBMW installs no crash signal handler on OHOS (crash_signal_handling.cc),
// so the tests that crash and wait for the report have nothing to check there.
#if defined(__OHOS__) || defined(__OHOS)
  #define IBMW_SKIP_WITHOUT_CRASH_HANDLER() GTEST_SKIP() << "no crash signal handler on OHOS"
#else
  #define IBMW_SKIP_WITHOUT_CRASH_HANDLER() static_cast<void>(0)
#endif

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
  IBMW_SKIP_WITHOUT_CRASH_HANDLER();
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
// crashing thread). Waiting there would take the whole kCrashFlushTimeout
// (the flush task can never run), so ending sooner proves there was no wait.
// The time also holds the report itself (about 250 ms with backward-cpp).
// CH-48
TEST_F(CrashLogFlushDeathTest, CrashOnLogExecutorThreadDoesNotWait) {
  IBMW_SKIP_WITHOUT_CRASH_HANDLER();
  const std::string dir = LogDir();
  EXPECT_EXIT(
      {
        auto* setup = new CrashLogSetup(dir);
        setup->exe.Dispatch([dir]() { CrashNowTimed(dir); });
        for (;;) std::this_thread::sleep_for(1s);
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  EXPECT_LT(Millis(SinceCrash(dir)), LoggingSubsystem::kCrashFlushTimeout.count());
}

// A stuck log executor: the flush gives up after kCrashFlushTimeout and the
// process ends with its signal.
// CH-48
TEST_F(CrashLogFlushDeathTest, StuckLogExecutorDelaysCrashOnlyByFlushTimeout) {
  IBMW_SKIP_WITHOUT_CRASH_HANDLER();
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

// dir/name; the death test child, which runs the test body again, finds it.
std::string MakeSubDir(const std::string& dir, const char* name) {
  const std::string sub = dir + "/" + name;
  std::error_code ec;
  std::filesystem::create_directory(sub, ec);
  EXPECT_TRUE(std::filesystem::is_directory(sub)) << sub;
  return sub;
}

// Which owner's record is published. Initialize() runs once per object, so
// "crash log off" is always a different object than the one that turned it
// on: it must not take that owner's hook away (its handler and logger stay),
// and each owner removes only its own record when it goes away. The child
// reports the first failed check in its exit code.
// CH-56
TEST_F(CrashLogFlushDeathTest, HookIsOwnedByThePublishingSubsystem) {
  const std::string dir = LogDir();
  const std::string a_dir = MakeSubDir(dir, "a");
  const std::string b_dir = MakeSubDir(dir, "b");
  const std::string c_dir = MakeSubDir(dir, "c");
  EXPECT_EXIT(
      {
        auto owner = []() -> const void* {
          const auto* hook = CrashSignalHandling::GetCrashFlush();
          return hook == nullptr ? nullptr : hook->ctx;
        };
        auto* a = new CrashLogSetup(a_dir);
        if (owner() != &a->logging) _exit(10);
        auto* b = new CrashLogSetup(b_dir, /*crash_log=*/false);
        if (owner() != &a->logging) _exit(11);  // crash log off: A's hook stays
        auto* c = new CrashLogSetup(c_dir);
        if (owner() != &c->logging) _exit(12);  // the last one on wins
        delete a;
        if (owner() != &c->logging) _exit(13);  // A's record was not published
        delete b;
        if (owner() != &c->logging) _exit(14);
        delete c;
        if (owner() != nullptr) _exit(15);  // C removed its own record
        _exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

// End to end: with an earlier owner's crash log on, initializing another
// subsystem with it off keeps the earlier hook, so the report queued behind
// the busy log executor still reaches the earlier owner's log.
// CH-56
TEST_F(CrashLogFlushDeathTest, CrashLogOffElsewhereKeepsEarlierHook) {
  IBMW_SKIP_WITHOUT_CRASH_HANDLER();
  const std::string dir = LogDir();
  const std::string off_dir = MakeSubDir(dir, "off");
  EXPECT_EXIT(
      {
        auto* first = new CrashLogSetup(dir);
        (void)new CrashLogSetup(off_dir, /*crash_log=*/false);
        first->exe.Dispatch([]() { std::this_thread::sleep_for(150ms); });
        CrashNow();
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  const std::string log = ReadFile(dir + "/crash.log");
  EXPECT_TRUE(LogHasCompleteReport(log)) << log;
}

// Two owners with the crash log on: destroying the earlier one leaves the
// later one's hook, which still flushes its queued report.
// CH-56
TEST_F(CrashLogFlushDeathTest, DestroyingEarlierOwnerKeepsLaterHook) {
  IBMW_SKIP_WITHOUT_CRASH_HANDLER();
  const std::string dir = LogDir();
  const std::string first_dir = MakeSubDir(dir, "first");
  EXPECT_EXIT(
      {
        auto* first = new CrashLogSetup(first_dir);
        auto* second = new CrashLogSetup(dir);
        delete first;
        second->exe.Dispatch([]() { std::this_thread::sleep_for(150ms); });
        CrashNow();
      },
      ::testing::KilledBySignal(SIGSEGV), "");
  const std::string log = ReadFile(dir + "/crash.log");
  EXPECT_TRUE(LogHasCompleteReport(log)) << log;
  EXPECT_FALSE(LogHasCompleteReport(ReadFile(first_dir + "/crash.log")));
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
