// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CORE-005: the crash handler puts back what it replaced and detaches its
// logger when destroyed.
// CORE-007: executor threads get their own alternate signal stack, so a stack
// overflow there is still reported.
// CORE-004: the raw part of the report (fault address and an unsymbolized
// trace) is written before the symbolizing part, which may never finish.

#include "engine/logger/crash_signal_handling.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "engine/executor/thread_pool_executor.h"
#include "engine/thread_tools.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::logger {
namespace {

void CustomHandler(int) {}

TEST(CrashSignalHandlingTest, DestructorRestoresHandlersAndAltStack) {
  struct sigaction custom {};
  custom.sa_handler = &CustomHandler;
  sigemptyset(&custom.sa_mask);
  struct sigaction saved {};
  ASSERT_EQ(sigaction(SIGSEGV, &custom, &saved), 0);
  stack_t before{};
  ASSERT_EQ(sigaltstack(nullptr, &before), 0);

  {
    CrashSignalHandling handling;
    struct sigaction installed {};
    ASSERT_EQ(sigaction(SIGSEGV, nullptr, &installed), 0);
    EXPECT_NE(installed.sa_flags & SA_SIGINFO, 0);
    stack_t during{};
    ASSERT_EQ(sigaltstack(nullptr, &during), 0);
    EXPECT_EQ(during.ss_flags & SS_DISABLE, 0);
    EXPECT_NE(during.ss_sp, before.ss_sp);
  }

  struct sigaction after {};
  ASSERT_EQ(sigaction(SIGSEGV, nullptr, &after), 0);
  EXPECT_EQ(after.sa_flags & SA_SIGINFO, 0);
  EXPECT_EQ(after.sa_handler, &CustomHandler);
  stack_t restored{};
  ASSERT_EQ(sigaltstack(nullptr, &restored), 0);
  EXPECT_EQ(restored.ss_flags & SS_DISABLE, before.ss_flags & SS_DISABLE);
  if ((before.ss_flags & SS_DISABLE) == 0) EXPECT_EQ(restored.ss_sp, before.ss_sp);
  ASSERT_EQ(sigaction(SIGSEGV, &saved, nullptr), 0);
}

TEST(CrashSignalHandlingTest, DestructorDetachesLogger) {
  auto logger = std::make_shared<ibmw::common::util::LoggerWrapper>();
  {
    CrashSignalHandling handling;
    CrashSignalHandling::SetLogger(logger);
    EXPECT_EQ(&CrashSignalHandling::GetLogger(), logger.get());
  }
  EXPECT_NE(&CrashSignalHandling::GetLogger(), logger.get());
  EXPECT_EQ(logger.use_count(), 1);
}

// A crash after the owner is gone takes the default action and no longer
// runs the IBMW handler.
TEST(CrashSignalHandlingTest, CrashAfterDestructionUsesPreviousHandler) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    signal(SIGSEGV, SIG_DFL);
    { CrashSignalHandling handling; }
    raise(SIGSEGV);
    _exit(0);
  }
  close(fds[1]);
  std::string output;
  char buffer[256];
  for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, n);
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
  EXPECT_EQ(output.find("IBMW crash signal"), std::string::npos) << output;
}

// The symbolizing part ends the process at once here, as a hang there would:
// what was written before it is the raw part.
TEST(CrashSignalHandlingTest, RawTraceIsWrittenBeforeTheSymbolizedReport) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    CrashSignalHandling handling;
    auto logger = std::make_shared<ibmw::common::util::LoggerWrapper>();
    logger->get_log_level_func = [] { return 0u; };
    logger->log_func = [](uint32_t, uint32_t, const char*, const char*, const char*, size_t) { _exit(42); };
    CrashSignalHandling::SetLogger(logger);
    *reinterpret_cast<volatile int*>(0x10) = 1;
    _exit(0);
  }
  close(fds[1]);
  std::string output;
  char buffer[4096];
  for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, n);
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 42) << output;  // the symbolizing part was reached
  EXPECT_NE(output.find("IBMW crash signal: 11\nIBMW crash pc: 0x"), std::string::npos) << output;
  EXPECT_NE(output.find(" addr: 0x10 code: 0x1\n"), std::string::npos) << output;  // SEGV_MAPERR
  // backtrace_symbols_fd(): one "...[0x...]" line per frame, the test among them.
  EXPECT_NE(output.find("[0x"), std::string::npos) << output;
  EXPECT_NE(output.find("ibmw_core_engine_test"), std::string::npos) << output;
}

// Even writing the report can block (stderr a full pipe nobody reads): the
// watchdog still ends the process with the original signal (~10 s).
TEST(CrashSignalHandlingTest, BlockedStderrStillEndsTheProcess) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    int fds[2];
    if (pipe(fds) != 0) _exit(1);
    (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
    static char fill[4096];
    while (write(fds[1], fill, sizeof(fill)) > 0) {
    }
    (void)fcntl(fds[1], F_SETFL, 0);
    dup2(fds[1], STDERR_FILENO);  // fds[0] stays open and unread
    CrashSignalHandling handling;
    raise(SIGSEGV);
    _exit(0);
  }
  int status = 0;
  pid_t done = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto limit = start + std::chrono::seconds(30);
  while ((done = waitpid(child, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < limit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (done == 0) {
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    FAIL() << "the crashed process hung";
  }
  ASSERT_EQ(done, child);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::seconds(9));  // it did block
}

bool CurrentThreadHasAltStack() {
  stack_t current{};
  return sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0;
}

TEST(CrashSignalHandlingTest, ExecutorThreadsGetAltStackOnlyWhileInstalled) {
  const auto worker_has_alt_stack = [] {
    executor::ThreadPoolExecutor executor;
    executor.Initialize("alt-stack-test", YAML::Load("{thread_num: 1}"));
    executor.Start();
    std::promise<bool> result;
    executor.Dispatch([&result] { result.set_value(CurrentThreadHasAltStack()); });
    const bool has = result.get_future().get();
    executor.Shutdown();
    return has;
  };
  EXPECT_FALSE(worker_has_alt_stack());
  {
    CrashSignalHandling handling;
    EXPECT_TRUE(worker_has_alt_stack());
  }
  EXPECT_FALSE(worker_has_alt_stack());
}

[[gnu::noinline]] int Recurse(int depth) {
  volatile char frame[1024];
  frame[0] = static_cast<char>(depth);
  return Recurse(depth + 1) + frame[0];
}

TEST(CrashSignalHandlingTest, StackOverflowOnWorkerThreadIsReported) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const rlimit no_core{0, 0};
    (void)setrlimit(RLIMIT_CORE, &no_core);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    CrashSignalHandling handling;
    std::thread([] {
      util::EnsureAltSignalStackForCurrentThread();
      Recurse(0);
    }).join();
    _exit(0);
  }
  close(fds[1]);
  std::string output;
  char buffer[4096];
  for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, n);
  close(fds[0]);
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);
  EXPECT_NE(output.find("IBMW crash signal: 11"), std::string::npos) << output.substr(0, 512);
  EXPECT_NE(output.find("IBMW crash pc: 0x"), std::string::npos) << output.substr(0, 512);
  EXPECT_NE(output.find(")[0x"), std::string::npos) << output.substr(0, 2048);  // backtrace() frames
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
