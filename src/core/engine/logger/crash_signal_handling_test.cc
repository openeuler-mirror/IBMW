// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CORE-005: the crash handler puts back what it replaced and detaches its
// logger when destroyed.

#include "engine/logger/crash_signal_handling.h"

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>

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

}  // namespace
}  // namespace ibmw::runtime::core::logger
