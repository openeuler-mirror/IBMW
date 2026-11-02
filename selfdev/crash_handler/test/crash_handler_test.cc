// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/crash_handler/crash_handler.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace crash_handler_test {

using namespace ibmw::selfdev::crash;

// ---- helpers kept out of line so they show up as distinct frames ----------

__attribute__((noinline)) size_t CaptureInner(void** frames, size_t n, size_t skip) {
  size_t r = CaptureStack(frames, n, skip);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) size_t CaptureMiddle(void** frames, size_t n, size_t skip) {
  size_t r = CaptureInner(frames, n, skip);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) size_t CaptureOuter(void** frames, size_t n, size_t skip) {
  size_t r = CaptureMiddle(frames, n, skip);
  asm volatile("" ::: "memory");
  return r;
}

volatile int* volatile g_null = nullptr;

// Not instrumented: under UBSan/ASan the null store would be reported (and
// the child exit with status 1) instead of raising a real SIGSEGV.
__attribute__((noinline, no_sanitize("address", "undefined"))) void CrashHere() {
  *g_null = 42;
  asm volatile("" ::: "memory");
}

volatile int g_recurse_limit = 1 << 30;  // never reached; keeps -Winfinite-recursion quiet

__attribute__((noinline)) int Recurse(int depth) {
  volatile char pad[1024];
  pad[0] = static_cast<char>(depth);
  if (depth >= g_recurse_limit) return pad[0];
  return Recurse(depth + 1) + pad[0];
}

struct ChildResult {
  int status = 0;
  std::string out;    // raw report fd
  std::string extra;  // second pipe, written by the report callback
};

int g_callback_fd = -1;

void CallbackToFd(const char* report, size_t len, void*) {
  if (g_callback_fd >= 0) {
    ssize_t r = write(g_callback_fd, report, len);
    (void)r;
  }
}

std::string ReadAll(int fd) {
  std::string s;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, static_cast<size_t>(n));
  return s;
}

// Runs body in a forked child. Options.output_fd is set to a pipe whose data
// ends up in ChildResult::out; g_callback_fd is a second pipe (extra). With
// kill_after_sec > 0 the child is killed with SIGKILL if it is still running
// after that many seconds (its output so far is kept).
ChildResult RunInChild(Options opt, const std::function<void()>& body, unsigned kill_after_sec = 0) {
  int out_pipe[2];
  int extra_pipe[2];
  EXPECT_EQ(pipe(out_pipe), 0);
  EXPECT_EQ(pipe(extra_pipe), 0);

  pid_t pid = fork();
  if (pid == 0) {
    close(out_pipe[0]);
    close(extra_pipe[0]);
    if (opt.output_fd != -1) opt.output_fd = out_pipe[1];
    g_callback_fd = extra_pipe[1];
    if (!Install(opt)) _exit(100);
    body();
    _exit(0);
  }

  close(out_pipe[1]);
  close(extra_pipe[1]);
  std::thread killer;
  std::atomic<bool> reaped{false};
  if (kill_after_sec > 0) {
    killer = std::thread([pid, kill_after_sec, &reaped] {
      for (unsigned i = 0; i < kill_after_sec * 10 && !reaped.load(); ++i) usleep(100 * 1000);
      if (!reaped.load()) kill(pid, SIGKILL);
    });
  }
  ChildResult r;
  r.out = ReadAll(out_pipe[0]);
  r.extra = ReadAll(extra_pipe[0]);
  close(out_pipe[0]);
  close(extra_pipe[0]);
  waitpid(pid, &r.status, 0);
  reaped.store(true);
  if (killer.joinable()) killer.join();
  return r;
}

size_t CountOccurrences(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t pos = s.find(needle); pos != std::string::npos; pos = s.find(needle, pos + 1)) ++n;
  return n;
}

// Index of the first stack line containing `needle`, or -1. Searches the
// symbolized part when the report has one, else the raw part.
int FrameIndexOf(const std::string& report, const std::string& needle) {
  size_t pos = report.find("*** symbolized stacktrace:");
  if (pos == std::string::npos) pos = 0;
  int index = 0;
  while (pos < report.size()) {
    size_t end = report.find('\n', pos);
    std::string line = report.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (line.rfind("#", 0) == 0) {
      if (line.find(needle) != std::string::npos) return index;
      ++index;
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return -1;
}

// ---- tests ------------------------------------------------------------------

// CH-01
TEST(CrashHandlerTest, DefaultSignals) {
  auto sigs = DefaultSignals();
  for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
    EXPECT_NE(std::find(sigs.begin(), sigs.end(), s), sigs.end()) << s;
  for (int s : {SIGINT, SIGTERM})
    EXPECT_EQ(std::find(sigs.begin(), sigs.end(), s), sigs.end()) << s;
}

// CH-02
TEST(CrashHandlerTest, SignalName) {
  EXPECT_STREQ(SignalName(SIGSEGV), "SIGSEGV");
  EXPECT_STREQ(SignalName(SIGABRT), "SIGABRT");
  EXPECT_STREQ(SignalName(9999), "UNKNOWN");
}

// CH-03
TEST(CrashHandlerTest, CaptureStackNamesInnermostFunction) {
  void* frames[32];
  size_t n = CaptureOuter(frames, 32, 0);
  ASSERT_GE(n, 3u);
  char buf[512];
  FormatFrame(buf, sizeof(buf), 0, frames[0], true, true);
  EXPECT_NE(std::string(buf).find("CaptureInner"), std::string::npos) << buf;
  FormatFrame(buf, sizeof(buf), 2, frames[2], true, true);
  EXPECT_NE(std::string(buf).find("CaptureOuter"), std::string::npos) << buf;
}

// CH-04
TEST(CrashHandlerTest, CaptureStackSkip) {
  void* a[32];
  void* b[32];
  size_t na = CaptureOuter(a, 32, 0);
  size_t nb = CaptureOuter(b, 32, 1);
  ASSERT_GE(na, 4u);
  ASSERT_EQ(nb, na - 1);
  // Frames inside the helper chain are shared; the frame for this test body
  // differs because the two CaptureOuter calls have different call sites.
  EXPECT_EQ(b[0], a[1]);
  EXPECT_EQ(b[1], a[2]);
}

// CH-05
TEST(CrashHandlerTest, CaptureStackBounds) {
  void* frames[2] = {reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2)};
  EXPECT_EQ(CaptureStack(frames, 0), 0u);
  EXPECT_EQ(frames[0], reinterpret_cast<void*>(0x1));
  EXPECT_EQ(CaptureStack(frames, 1), 1u);
  EXPECT_EQ(frames[1], reinterpret_cast<void*>(0x2));
  EXPECT_EQ(CaptureStack(nullptr, 4), 0u);
}

// CH-06
TEST(CrashHandlerTest, FormatFrameLayout) {
  char buf[512];
  void* pc = reinterpret_cast<void*>(&CrashHere);
  size_t len = FormatFrame(buf, sizeof(buf), 0, pc, true, false);
  std::string s(buf, len);
  EXPECT_EQ(s.rfind("#00 pc 0x", 0), 0u) << s;
  EXPECT_NE(s.find("crash_handler_test+0x"), std::string::npos) << s;
  EXPECT_EQ(s.back(), '\n');
  EXPECT_EQ(len, std::strlen(buf));
}

// CH-07
TEST(CrashHandlerTest, FormatFrameTruncates) {
  void* pc = reinterpret_cast<void*>(&CrashHere);
  for (size_t cap : {1u, 8u, 16u}) {
    char buf[32];
    std::memset(buf, 'x', sizeof(buf));
    size_t len = FormatFrame(buf, cap, 3, pc, true, false);
    EXPECT_LT(len, cap);
    EXPECT_EQ(buf[len], '\0');
    EXPECT_EQ(buf[cap], 'x') << "wrote past buffer, cap=" << cap;
  }
  EXPECT_EQ(FormatFrame(nullptr, 0, 0, pc, true, false), 0u);
}

// CH-08
TEST(CrashHandlerTest, FormatFrameWithoutSymbols) {
  char buf[256];
  FormatFrame(buf, sizeof(buf), 7, reinterpret_cast<void*>(0x1234), false, false);
  EXPECT_STREQ(buf, "#07 pc 0x0000000000001234\n");
}

// CH-09
TEST(CrashHandlerTest, FormatStackDemangles) {
  void* frames[1] = {reinterpret_cast<void*>(&CrashHere)};
  char buf[1024];
  FormatStack(buf, sizeof(buf), frames, 1, true, true);
  EXPECT_NE(std::string(buf).find("crash_handler_test::CrashHere"), std::string::npos) << buf;
  FormatStack(buf, sizeof(buf), frames, 1, true, false);
  EXPECT_NE(std::string(buf).find("_ZN18crash_handler_test9CrashHereEv"), std::string::npos) << buf;
}

// CH-10
TEST(CrashHandlerTest, InstallAndUninstallRestoresPreviousAction) {
  struct sigaction before;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &before), 0);

  Options opt;
  opt.signals = {SIGUSR2};
  opt.alt_stack_size = 0;
  ASSERT_TRUE(Install(opt));
  EXPECT_TRUE(IsInstalled());
  struct sigaction during;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &during), 0);
  EXPECT_TRUE(during.sa_flags & SA_SIGINFO);
  EXPECT_NE(reinterpret_cast<void*>(during.sa_sigaction), reinterpret_cast<void*>(before.sa_sigaction));

  Uninstall();
  EXPECT_FALSE(IsInstalled());
  struct sigaction after;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &after), 0);
  EXPECT_EQ(reinterpret_cast<void*>(after.sa_handler), reinterpret_cast<void*>(before.sa_handler));
}

// CH-11, CH-12
TEST(CrashHandlerTest, SegfaultReportsAndTerminatesWithSignal) {
  ChildResult r = RunInChild(Options{}, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
  EXPECT_NE(r.out.find("*** IBMW crash: signal 11 (SIGSEGV)"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("#00 pc 0x"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  int idx = FrameIndexOf(r.out, "CrashHere");
  EXPECT_GE(idx, 0) << r.out;
  EXPECT_LT(idx, 3) << r.out;
}

// CH-13
TEST(CrashHandlerTest, Abort) {
  ChildResult r = RunInChild(Options{}, [] { std::abort(); });
  ASSERT_TRUE(WIFSIGNALED(r.status));
  EXPECT_EQ(WTERMSIG(r.status), SIGABRT);
  EXPECT_NE(r.out.find("(SIGABRT)"), std::string::npos) << r.out;
}

// CH-14
TEST(CrashHandlerTest, Sigfpe) {
  ChildResult r = RunInChild(Options{}, [] { raise(SIGFPE); });
  ASSERT_TRUE(WIFSIGNALED(r.status));
  EXPECT_EQ(WTERMSIG(r.status), SIGFPE);
  EXPECT_NE(r.out.find("(SIGFPE)"), std::string::npos) << r.out;
}

// Clang 14's TSAN runtime sometimes deadlocks on one of its own futexes when
// the stack overflows inside instrumented code (about half the runs; the
// handler is entered but blocks before writing anything). Measured with and
// without this library's handler-entry sigmask change: same rate; GCC 11's
// TSAN and every non-TSAN build never hang. Such a child is killed by the
// deadline below and must have written nothing.
#if defined(__clang__) && defined(__has_feature)
  #if __has_feature(thread_sanitizer)
constexpr bool kTsanMayHangOnOverflow = true;
  #else
constexpr bool kTsanMayHangOnOverflow = false;
  #endif
#else
constexpr bool kTsanMayHangOnOverflow = false;
#endif

// CH-15 (5.6: deadline so a hang fails instead of blocking ctest)
TEST(CrashHandlerTest, StackOverflowUsesAltStack) {
  ChildResult r = RunInChild(Options{}, [] { (void)Recurse(0); }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status));
  if (kTsanMayHangOnOverflow && WTERMSIG(r.status) == SIGKILL) {
    EXPECT_TRUE(r.out.empty()) << r.out;
    std::fprintf(stderr, "note: clang TSAN runtime hung on stack overflow; child killed by the test deadline\n");
    return;
  }
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(r.out.find("(SIGSEGV)"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
}

// CH-16
TEST(CrashHandlerTest, ReportCallbackGetsDemangledReport) {
  Options opt;
  opt.report_callback = &CallbackToFd;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status));
  EXPECT_NE(r.extra.find("*** IBMW crash: signal 11 (SIGSEGV)"), std::string::npos) << r.extra;
  EXPECT_NE(r.extra.find("crash_handler_test::CrashHere"), std::string::npos) << r.extra;
  EXPECT_NE(r.out.find("_ZN18crash_handler_test9CrashHereEv"), std::string::npos) << r.out;
}

// CH-17
TEST(CrashHandlerTest, RawOutputCanBeDisabled) {
  Options opt;
  opt.output_fd = -1;
  opt.report_callback = &CallbackToFd;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status));
  EXPECT_TRUE(r.out.empty()) << r.out;
  EXPECT_NE(r.extra.find("(SIGSEGV)"), std::string::npos);
}

// CH-18
TEST(CrashHandlerTest, NoReraiseExitsWith128PlusSignal) {
  Options opt;
  opt.reraise = false;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFEXITED(r.status));
  EXPECT_EQ(WEXITSTATUS(r.status), 128 + SIGSEGV);
}

// CH-19
TEST(CrashHandlerTest, ConcurrentCrashesReportOnce) {
  ChildResult r = RunInChild(Options{}, [] {
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([] { CrashHere(); });
    for (auto& t : threads) t.join();
  });
  ASSERT_TRUE(WIFSIGNALED(r.status));
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
  EXPECT_EQ(CountOccurrences(r.out, "*** IBMW crash:"), 1u) << r.out;
}

// CH-22
void HangingCallback(const char*, size_t, void*) {
  for (;;) pause();
}

TEST(CrashHandlerTest, WatchdogEndsHangingReport) {
  Options opt;
  opt.report_callback = &HangingCallback;
  opt.report_timeout_sec = 1;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: report timed out"), std::string::npos) << r.out;
}

// CH-23
TEST(CrashHandlerTest, DuplicateSignalsStillRestorePreviousAction) {
  struct sigaction before;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &before), 0);

  Options opt;
  opt.signals = {SIGUSR2, SIGUSR2};
  opt.alt_stack_size = 0;
  ASSERT_TRUE(Install(opt));
  Uninstall();
  struct sigaction after;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &after), 0);
  EXPECT_FALSE(after.sa_flags & SA_SIGINFO);
  EXPECT_EQ(reinterpret_cast<void*>(after.sa_handler), reinterpret_cast<void*>(before.sa_handler));
}

// CH-24
TEST(CrashHandlerTest, ReinstallThenUninstallRestoresOriginal) {
  struct sigaction before;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &before), 0);

  Options opt;
  opt.signals = {SIGUSR2};
  opt.alt_stack_size = 0;
  ASSERT_TRUE(Install(opt));
  ASSERT_TRUE(Install(opt));
  Uninstall();
  struct sigaction after;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &after), 0);
  EXPECT_EQ(reinterpret_cast<void*>(after.sa_handler), reinterpret_cast<void*>(before.sa_handler));
}

// CH-25
TEST(CrashHandlerTest, NotInstalledWhenEverySigactionFails) {
  Options opt;
  opt.signals = {SIGKILL, SIGSTOP};
  opt.alt_stack_size = 0;
  EXPECT_FALSE(Install(opt));
  EXPECT_FALSE(IsInstalled());
  Uninstall();
}

// CH-26
TEST(CrashHandlerTest, SignalNameCoversDefaultSignals) {
  EXPECT_STREQ(SignalName(SIGBUS), "SIGBUS");
  EXPECT_STREQ(SignalName(SIGQUIT), "SIGQUIT");
  EXPECT_STREQ(SignalName(SIGSYS), "SIGSYS");
  EXPECT_STREQ(SignalName(SIGTRAP), "SIGTRAP");
  EXPECT_STREQ(SignalName(SIGXCPU), "SIGXCPU");
  EXPECT_STREQ(SignalName(SIGXFSZ), "SIGXFSZ");
  EXPECT_STREQ(SignalName(SIGFPE), "SIGFPE");
  EXPECT_STREQ(SignalName(SIGILL), "SIGILL");
  for (int sig : DefaultSignals()) EXPECT_STRNE(SignalName(sig), "UNKNOWN") << sig;
}

// CH-27
void CrashingCallback(const char*, size_t, void*) { CrashHere(); }

// Updated after review: a second fault inside the handler ends the process
// as the original signal and honours reraise (CH-30 covers reraise=false);
// it used to force reraise on and re-raise the second signal.
TEST(CrashHandlerTest, CrashInsideCallbackStillTerminatesWithSignal) {
  Options opt;
  opt.report_callback = &CrashingCallback;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
  EXPECT_EQ(CountOccurrences(r.out, "*** IBMW crash: signal"), 1u) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  // Round 2: the nested fault says which stage it hit.
  EXPECT_NE(r.out.find("*** IBMW crash: report callback faulted"), std::string::npos) << r.out;
}

// CH-28
TEST(CrashHandlerTest, SingleFrameReportStartsAtFaultingPc) {
  Options opt;
  opt.max_frames = 1;  // only the handler's own frame fits: pc is prepended
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CrashHere"), 0) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace (1 frames)"), std::string::npos) << r.out;
}

// CH-29: the raw part (header + module+offset frames from the module table
// recorded by Install(), no dladdr) comes first; the symbolized part follows.
TEST(CrashHandlerTest, RawPartPrecedesSymbolizedPart) {
  ChildResult r = RunInChild(Options{}, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  const size_t header = r.out.find("*** IBMW crash: signal 11 (SIGSEGV)");
  const size_t raw = r.out.find("*** raw stacktrace (no symbol lookup):\n#00 pc 0x");
  const size_t sym = r.out.find("*** symbolized stacktrace:\n#00 pc 0x");
  ASSERT_NE(header, std::string::npos) << r.out;
  ASSERT_NE(raw, std::string::npos) << r.out;
  ASSERT_NE(sym, std::string::npos) << r.out;
  EXPECT_LT(header, raw);
  EXPECT_LT(raw, sym);
  // Raw frames name the test executable through the module table.
  const std::string raw_part = r.out.substr(raw, sym - raw);
  EXPECT_NE(raw_part.find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos) << raw_part;
  EXPECT_NE(raw_part.find(" [base 0x"), std::string::npos) << raw_part;
  EXPECT_EQ(raw_part.find("CrashHere"), std::string::npos) << "raw part must not use symbols: " << raw_part;
}

// CH-29b: symbolize = false gives the raw part alone (module+offset still
// present, from the table), ending the report.
TEST(CrashHandlerTest, RawOnlyReportWithoutSymbolize) {
  Options opt;
  opt.symbolize = false;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(r.out.find("*** symbolized"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace ("), std::string::npos) << r.out;
}

// CH-30: reraise = false and a second fault inside the handler: exit status
// 128 + the original signal.
TEST(CrashHandlerTest, NestedCrashWithoutReraiseExits128PlusOriginal) {
  Options opt;
  opt.report_callback = &CrashingCallback;
  opt.reraise = false;
  ChildResult r = RunInChild(opt, [] { std::abort(); });
  ASSERT_TRUE(WIFEXITED(r.status)) << r.out;
  EXPECT_EQ(WEXITSTATUS(r.status), 128 + SIGABRT) << r.out;
  EXPECT_NE(r.out.find("(SIGABRT)"), std::string::npos) << r.out;
}

// CH-31: abort() inside the callback: the process still ends with the
// original SIGSEGV, not SIGABRT.
void AbortingCallback(const char*, size_t, void*) { std::abort(); }

TEST(CrashHandlerTest, AbortInsideCallbackKeepsOriginalSignal) {
  Options opt;
  opt.report_callback = &AbortingCallback;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_EQ(CountOccurrences(r.out, "*** IBMW crash: signal"), 1u) << r.out;
}

// CH-32: a real (synchronous) SIGFPE inside the callback. SIGFPE is not in
// the handler's sa_mask, so it re-enters the handler instead of the kernel
// killing the process with SIGFPE; the exit reflects the original SIGSEGV.
volatile int g_zero = 0;

__attribute__((noinline, no_sanitize("undefined"))) int DivideByZero() {
#if defined(__x86_64__) || defined(__i386__)
  volatile int one = 1;
  return one / g_zero;  // idiv traps with SIGFPE
#else
  // Integer division does not trap on this architecture.
  raise(SIGFPE);
  return 0;
#endif
}

void FpeCallback(const char*, size_t, void*) { (void)DivideByZero(); }

TEST(CrashHandlerTest, SigfpeInsideCallbackKeepsOriginalSignal) {
  Options opt;
  opt.report_callback = &FpeCallback;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
}

// The path CMake built a test library at, or, when the tests were copied to
// another machine (a cross build run on the board), the same file next to the
// test executable.
std::string TestModulePath(const char* built) {
  if (access(built, R_OK) == 0) return built;
  char exe[4096];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return built;
  const std::string exe_path(exe, static_cast<size_t>(n));
  const std::string built_path(built);
  const size_t exe_slash = exe_path.rfind('/');
  const size_t built_slash = built_path.rfind('/');
  if (exe_slash == std::string::npos || built_slash == std::string::npos) return built;
  return exe_path.substr(0, exe_slash) + built_path.substr(built_slash);
}

// CH-33: one thread holds the loader lock (inside the constructor of a
// dlopen'ed test library, which blocks forever) while another thread
// crashes. dladdr would block; the header and the raw frames are written
// before any symbol lookup, and the watchdog ends the process.
//
// Under TSAN the watchdog cannot fire here: TSAN queues asynchronous signals
// (SIGALRM) until the thread next enters an intercepted call, and the
// reporting thread stays blocked in the loader's internal lock. The test
// then ends the child itself after a deadline and checks the raw part,
// which is the property under test; the watchdog is covered by CH-22.
#if defined(__SANITIZE_THREAD__)
constexpr bool kAsyncSignalsDeferred = true;
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
constexpr bool kAsyncSignalsDeferred = true;
  #else
constexpr bool kAsyncSignalsDeferred = false;
  #endif
#else
constexpr bool kAsyncSignalsDeferred = false;
#endif

TEST(CrashHandlerTest, LoaderLockHeldStillWritesRawReport) {
  Options opt;
  opt.report_timeout_sec = 1;
  ChildResult r = RunInChild(opt, [] {
    int ready[2];
    if (pipe(ready) != 0) _exit(101);
    std::string fd = std::to_string(ready[1]);
    setenv("IBMW_CRASH_TEST_CTOR_FD", fd.c_str(), 1);
    std::thread([] { (void)dlopen(TestModulePath(IBMW_CRASH_TEST_BLOCKING_SO).c_str(), RTLD_NOW); }).detach();
    char c = 0;
    if (read(ready[0], &c, 1) != 1) _exit(102);  // the constructor is running
    CrashHere();
  }, /*kill_after_sec=*/8);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: signal 11 (SIGSEGV)"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** raw stacktrace (no symbol lookup):\n#00 pc 0x"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos) << r.out;
#if defined(__OHOS__)
  // OHOS's dladdr does not wait for the loader lock: the report completes.
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  return;
#endif
  // dladdr stayed blocked: the symbolized part never started.
  EXPECT_EQ(r.out.find("*** symbolized stacktrace:"), std::string::npos) << r.out;
  if (kAsyncSignalsDeferred) {
    EXPECT_EQ(WTERMSIG(r.status), SIGKILL) << r.out;
  } else {
    EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
    EXPECT_NE(r.out.find("*** IBMW crash: report timed out"), std::string::npos) << r.out;
  }
}

// CH-34: Uninstall() leaves alone a handler installed after Install().
void OtherHandler(int) {}

TEST(CrashHandlerTest, UninstallKeepsLaterHandler) {
  struct sigaction before;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &before), 0);
  Options opt;
  opt.signals = {SIGUSR2};
  opt.alt_stack_size = 0;
  ASSERT_TRUE(Install(opt));
  struct sigaction other;
  std::memset(&other, 0, sizeof(other));
  other.sa_handler = &OtherHandler;
  sigemptyset(&other.sa_mask);
  ASSERT_EQ(sigaction(SIGUSR2, &other, nullptr), 0);
  Uninstall();
  struct sigaction after;
  ASSERT_EQ(sigaction(SIGUSR2, nullptr, &after), 0);
  EXPECT_EQ(reinterpret_cast<void*>(after.sa_handler), reinterpret_cast<void*>(&OtherHandler));
  ASSERT_EQ(sigaction(SIGUSR2, &before, nullptr), 0);
}

// CH-35: the handler's sa_mask blocks asynchronous signals but none of the
// synchronous ones.
TEST(CrashHandlerTest, SaMaskExcludesSynchronousSignals) {
  struct sigaction before;
  ASSERT_EQ(sigaction(SIGSEGV, nullptr, &before), 0);
  Options opt;
  opt.signals = {SIGSEGV};
  opt.alt_stack_size = 0;
  ASSERT_TRUE(Install(opt));
  struct sigaction now;
  ASSERT_EQ(sigaction(SIGSEGV, nullptr, &now), 0);
  Uninstall();
  for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP}) EXPECT_EQ(sigismember(&now.sa_mask, sig), 0) << sig;
  for (int sig : {SIGINT, SIGTERM, SIGUSR1, SIGALRM}) EXPECT_EQ(sigismember(&now.sa_mask, sig), 1) << sig;
}

// ---- calls to a pc that is not code (5.6 re-review round 2) ---------------

using Fn = void (*)();
volatile Fn g_null_fn = nullptr;
volatile Fn g_wild_fn = reinterpret_cast<Fn>(0x1234);

__attribute__((noinline, no_sanitize("address", "undefined"))) void CallThroughPointer(volatile Fn* fn) {
  (*fn)();
  asm volatile("" ::: "memory");
}

void ExpectReportNamesCaller(const ChildResult& r, const std::string& pc_hex) {
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: signal 11 (SIGSEGV)"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** pc 0x" + pc_hex + "  (not in a loaded module)"), std::string::npos) << r.out;
#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
  // The caller line comes from the interrupted context ([sp] / LR) and names
  // the test executable through the module table.
  const size_t caller = r.out.find("*** caller 0x");
  ASSERT_NE(caller, std::string::npos) << r.out;
  const std::string caller_line = r.out.substr(caller, r.out.find('\n', caller) - caller);
  EXPECT_NE(caller_line.find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos) << caller_line;
  // The stack walk restarts at the caller: frame #00 is the bad pc, frame #01
  // the function that made the call.
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("faulted"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("#00 pc 0x" + pc_hex), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CallThroughPointer"), 1) << r.out;
#endif
}

// CH-36: call through a null function pointer: the header (pc 0, caller)
// and a full stacktrace from the caller on.
TEST(CrashHandlerTest, NullFunctionPointerCallNamesCaller) {
  ChildResult r = RunInChild(Options{}, [] { CallThroughPointer(&g_null_fn); }, /*kill_after_sec=*/15);
  ExpectReportNamesCaller(r, "0000000000000000");
}

// CH-37: call through a wild function pointer (0x1234). libgcc's unwinder
// reads the instructions at an unknown pc and faulted before anything was
// written; now the header goes out first and the walk starts at the caller.
TEST(CrashHandlerTest, WildFunctionPointerCallNamesCaller) {
  ChildResult r = RunInChild(Options{}, [] { CallThroughPointer(&g_wild_fn); }, /*kill_after_sec=*/15);
  ExpectReportNamesCaller(r, "0000000000001234");
}

#if defined(__x86_64__) && defined(__linux__)
// Jumps to 0x1234 with 0x5678 at [sp]: the "caller" is not code either, so
// the stack walk itself faults.
__attribute__((noinline, no_sanitize("address", "undefined"))) void JumpToWildWithJunkReturn() {
  asm volatile(
      "pushq $0x5678\n\t"
      "movq $0x1234, %%rax\n\t"
      "jmp *%%rax\n\t" ::: "rax", "memory");
}

// CH-38: a fault inside the stack walk: the header (with the caller line) is
// already out, a marker says the capture faulted, and the process still ends
// with the original signal (reraise = false: 128 + SIGSEGV).
TEST(CrashHandlerTest, FaultInStackCaptureLeavesHeaderAndMarker) {
  Options opt;
  opt.reraise = false;
  ChildResult r = RunInChild(opt, [] { JumpToWildWithJunkReturn(); }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFEXITED(r.status)) << r.out;
  EXPECT_EQ(WEXITSTATUS(r.status), 128 + SIGSEGV) << r.out;
  const size_t header = r.out.find("*** IBMW crash: signal 11 (SIGSEGV)");
  const size_t caller = r.out.find("*** caller 0x0000000000005678  (not in a loaded module)");
  const size_t marker = r.out.find("*** IBMW crash: stack capture faulted, no stacktrace");
  ASSERT_NE(header, std::string::npos) << r.out;
  ASSERT_NE(caller, std::string::npos) << r.out;
  ASSERT_NE(marker, std::string::npos) << r.out;
  EXPECT_LT(header, caller);
  EXPECT_LT(caller, marker);
  EXPECT_EQ(CountOccurrences(r.out, "*** IBMW crash: signal"), 1u) << r.out;
}
#endif

// ---- caller logic and late-loaded modules (5.6 re-review round 3) --------

void* LateSymbol(const char* name) {
  void* h = dlopen(TestModulePath(IBMW_CRASH_TEST_LATE_SO).c_str(), RTLD_NOW);
  if (h == nullptr) _exit(103);
  void* sym = dlsym(h, name);
  if (sym == nullptr) _exit(104);
  return sym;
}

std::string HeaderLine(const std::string& out, const std::string& prefix) {
  const size_t pos = out.find(prefix);
  if (pos == std::string::npos) return {};
  return out.substr(pos, out.find('\n', pos) - pos);
}

void ExpectLateOverflowReport(const ChildResult& r, bool refreshed) {
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  if (kTsanMayHangOnOverflow && WTERMSIG(r.status) == SIGKILL) {
    std::fprintf(stderr, "note: clang TSAN runtime hung on stack overflow; child killed by the test deadline\n");
    return;
  }
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: signal 11 (SIGSEGV)"), std::string::npos) << r.out;
  const std::string pc_line = HeaderLine(r.out, "*** pc 0x");
  if (refreshed) {
    EXPECT_NE(pc_line.find("libibmw_selfdev_crash_handler_late_lib.so+0x"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("libibmw_selfdev_crash_handler_late_lib.so+0x", r.out.find("*** raw stacktrace")),
              std::string::npos) << r.out;
  } else {
    EXPECT_NE(pc_line.find("(not in a loaded module)"), std::string::npos) << r.out;
  }
  // A stack overflow is a data fault on the guard page: [sp] is not read
  // (it would fault again) and no caller line is printed.
  EXPECT_EQ(r.out.find("*** caller"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("faulted"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** raw stacktrace (no symbol lookup):\n#00 pc 0x"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CrashTestLateRecurse"), 0) << r.out;
}

// CH-39: stack overflow in a library dlopen'ed after Install() (an IBMW
// package): the full report, pc named by the symbolized part only.
TEST(CrashHandlerTest, StackOverflowInLateLibraryWritesFullReport) {
  ChildResult r = RunInChild(Options{}, [] {
    auto* recurse = reinterpret_cast<int (*)(int)>(LateSymbol("CrashTestLateRecurse"));
    (void)recurse(0);
  }, /*kill_after_sec=*/15);
  ExpectLateOverflowReport(r, /*refreshed=*/false);
}

// CH-40: same after RefreshModules(): header and raw frames name the library.
TEST(CrashHandlerTest, RefreshModulesNamesLateLibrary) {
  ChildResult r = RunInChild(Options{}, [] {
    auto* recurse = reinterpret_cast<int (*)(int)>(LateSymbol("CrashTestLateRecurse"));
    RefreshModules();
    (void)recurse(0);
  }, /*kill_after_sec=*/15);
  ExpectLateOverflowReport(r, /*refreshed=*/true);
}

__attribute__((noinline)) void CallLateNullWrite(void (*fn)()) {
  fn();
  asm volatile("" ::: "memory");
}

// CH-41: an ordinary null store in a late library ([sp] holds junk such as
// 0x1111): no caller line, the walk starts at the faulting pc.
TEST(CrashHandlerTest, DataFaultInLateLibraryHasNoCallerLine) {
  ChildResult r = RunInChild(Options{}, [] {
    CallLateNullWrite(reinterpret_cast<void (*)()>(LateSymbol("CrashTestLateNullWrite")));
  }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(r.out.find("fault addr 0x0000000000000000"), std::string::npos) << r.out;
  EXPECT_NE(HeaderLine(r.out, "*** pc 0x").find("(not in a loaded module)"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("*** caller"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("faulted"), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CrashTestLateNullWrite"), 0) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CallLateNullWrite"), 1) << r.out;
}

#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
alignas(64) unsigned char g_data_not_code[64];

__attribute__((noinline, no_sanitize("address", "undefined"))) void CallIntoData() {
  reinterpret_cast<Fn>(static_cast<void*>(g_data_not_code))();
  asm volatile("" ::: "memory");
}

// CH-42: a call into data inside a loaded module (.bss of the test program):
// pc is in the module table, the fault is still the instruction fetch, so the
// caller line and the walk from the caller are kept.
TEST(CrashHandlerTest, CallIntoDataInsideModuleNamesCaller) {
  ChildResult r = RunInChild(Options{}, [] { CallIntoData(); }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(HeaderLine(r.out, "*** pc 0x").find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos)
      << r.out;
  EXPECT_NE(HeaderLine(r.out, "*** caller 0x").find("ibmw_selfdev_crash_handler_test+0x"), std::string::npos)
      << r.out;
  EXPECT_EQ(r.out.find("faulted"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "g_data_not_code"), 0) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CallIntoData"), 1) << r.out;
}

// CH-43: the handler moves pc/sp to the caller for the walk only. A wrapper
// installed in front of HandleSignal records the registers the kernel saved;
// the report callback (which runs after the walk) checks they are back, so
// a core dump or a debugger sees the faulting pc 0x1234 and its sp.
struct sigaction g_inner_action;
ucontext_t* g_wrapped_ctx = nullptr;
uintptr_t g_fault_pc = 0;
uintptr_t g_fault_sp = 0;

uintptr_t CtxPc(const ucontext_t* c) {
  #if defined(__x86_64__)
  return static_cast<uintptr_t>(c->uc_mcontext.gregs[REG_RIP]);
  #else
  return static_cast<uintptr_t>(c->uc_mcontext.pc);
  #endif
}

uintptr_t CtxSp(const ucontext_t* c) {
  #if defined(__x86_64__)
  return static_cast<uintptr_t>(c->uc_mcontext.gregs[REG_RSP]);
  #else
  return static_cast<uintptr_t>(c->uc_mcontext.sp);
  #endif
}

void RecordingWrapper(int signo, siginfo_t* info, void* ctx) {
  if (g_wrapped_ctx == nullptr) {
    g_wrapped_ctx = static_cast<ucontext_t*>(ctx);
    g_fault_pc = CtxPc(g_wrapped_ctx);
    g_fault_sp = CtxSp(g_wrapped_ctx);
  }
  g_inner_action.sa_sigaction(signo, info, ctx);
}

void CheckContextRestored(const char*, size_t, void*) {
  char line[160];
  const int n = std::snprintf(line, sizeof line, "fault pc %#lx, pc restored %d, sp restored %d\n",
                              static_cast<unsigned long>(g_fault_pc),
                              CtxPc(g_wrapped_ctx) == g_fault_pc ? 1 : 0,
                              CtxSp(g_wrapped_ctx) == g_fault_sp ? 1 : 0);
  if (g_callback_fd >= 0 && n > 0) {
    ssize_t w = write(g_callback_fd, line, static_cast<size_t>(n));
    (void)w;
  }
}

TEST(CrashHandlerTest, InterruptedContextIsRestoredAfterTheWalk) {
  Options opt;
  opt.report_callback = &CheckContextRestored;
  ChildResult r = RunInChild(opt, [] {
    if (sigaction(SIGSEGV, nullptr, &g_inner_action) != 0) _exit(105);
    if ((g_inner_action.sa_flags & SA_SIGINFO) == 0) _exit(106);
    struct sigaction wrapper = g_inner_action;
    wrapper.sa_sigaction = &RecordingWrapper;
    if (sigaction(SIGSEGV, &wrapper, nullptr) != 0) _exit(107);
    CallThroughPointer(&g_wild_fn);
  }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out << r.extra;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  // The context was moved for the walk (caller line, frame #01 the caller) ...
  EXPECT_NE(r.out.find("*** caller 0x"), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CallThroughPointer"), 1) << r.out;
  // ... and put back before the callback.
  EXPECT_EQ(r.extra, "fault pc 0x1234, pc restored 1, sp restored 1\n") << r.out;
}
#endif

// ---- module table limits and concurrent refresh (5.6 re-review round 4) --

#if defined(__linux__)
// Loads one more copy of the late library from a memfd: a new inode, so the
// loader maps it again instead of returning the already loaded one.
void LoadLateCopy() {
  static std::string image;
  if (image.empty()) {
    const int src = open(TestModulePath(IBMW_CRASH_TEST_LATE_SO).c_str(), O_RDONLY | O_CLOEXEC);
    if (src < 0) _exit(108);
    image = ReadAll(src);
    close(src);
    if (image.empty()) _exit(109);
  }
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "late_copy", 0));
  if (fd < 0) _exit(110);
  if (write(fd, image.data(), image.size()) != static_cast<ssize_t>(image.size())) _exit(111);
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
  if (dlopen(path, RTLD_NOW | RTLD_LOCAL) == nullptr) _exit(112);
}

// CH-45: more modules than the table holds (kMaxModules = 256): the report
// says how many were not recorded, instead of silently showing bare pcs.
TEST(CrashHandlerTest, ModuleTableFullIsReported) {
  ChildResult r = RunInChild(Options{}, [] {
    for (int i = 0; i < 270; ++i) LoadLateCopy();
    RefreshModules();
    CrashHere();
  }, /*kill_after_sec=*/60);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  const std::string marker = HeaderLine(r.out, "*** module table full: ");
  ASSERT_FALSE(marker.empty()) << r.out;
  const long dropped = std::strtol(marker.c_str() + std::strlen("*** module table full: "), nullptr, 10);
  // At least the copies beyond the cap, since the test program's own
  // modules are there too.
  EXPECT_GE(dropped, 270 - 256) << marker;
  EXPECT_NE(marker.find(" modules not recorded (their frames show bare pcs)"), std::string::npos) << marker;
  EXPECT_LT(r.out.find("*** raw stacktrace"), r.out.find("*** module table full: ")) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_EQ(FrameIndexOf(r.out, "CrashHere"), 0) << r.out;
}

// Without the cap exceeded there is no marker (any report of the suite).
// CH-45b
TEST(CrashHandlerTest, NoModuleTableFullMarkerBelowCap) {
  ChildResult r = RunInChild(Options{}, [] { CrashHere(); }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(r.out.find("module table full"), std::string::npos) << r.out;
}

// CH-46: a RefreshModules() running on another thread while a report is
// written. The table being rewritten must never give a wrong module name or
// offset: the pc line names the late library or, when a refresh overlapped
// the lookup, falls back to the bare pc. Repeated to hit the overlap.
TEST(CrashHandlerTest, ConcurrentRefreshNeverNamesWrongModule) {
  constexpr int kRuns = 20;
  int named = 0;
  int bare = 0;
  for (int run = 0; run < kRuns; ++run) {
    ChildResult r = RunInChild(Options{}, [] {
      auto* fn = reinterpret_cast<void (*)()>(LateSymbol("CrashTestLateNullWrite"));
      // The function's offset in the library, for the parent to compare.
      // Only the child loads the library: the other late-library tests
      // (also under --gtest_repeat) need it absent from the parent.
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(fn), &info) == 0) _exit(113);
      char line[64];
      const int n = std::snprintf(line, sizeof line, "%lx\n",
                                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(info.dli_saddr) -
                                                             reinterpret_cast<uintptr_t>(info.dli_fbase)));
      if (n <= 0 || write(g_callback_fd, line, static_cast<size_t>(n)) != n) _exit(114);
      RefreshModules();
      std::atomic<bool> started{false};
      std::thread([&started] {
        started.store(true);
        for (;;) RefreshModules();
      }).detach();
      while (!started.load()) usleep(100);
      CallLateNullWrite(fn);
    }, /*kill_after_sec=*/15);
    ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
    EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
    const uintptr_t fn_offset = std::strtoull(r.extra.c_str(), nullptr, 16);
    ASSERT_NE(fn_offset, 0u) << r.extra;
    const std::string pc_line = HeaderLine(r.out, "*** pc 0x");
    const bool is_named = pc_line.find("libibmw_selfdev_crash_handler_late_lib.so+0x") != std::string::npos;
    const bool is_bare = pc_line.find("(not in a loaded module)") != std::string::npos;
    EXPECT_TRUE(is_named || is_bare) << pc_line;
    if (is_named) {
      // Not a torn entry: the offset lies in CrashTestLateNullWrite's code.
      const size_t off_pos = pc_line.find("late_lib.so+0x") + std::strlen("late_lib.so+0x");
      const uintptr_t off = std::strtoull(pc_line.c_str() + off_pos, nullptr, 16);
      EXPECT_GE(off, fn_offset) << pc_line;
      EXPECT_LT(off, fn_offset + 0x400) << pc_line;
      ++named;
    } else if (is_bare) {
      ++bare;
    }
    EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
    EXPECT_EQ(FrameIndexOf(r.out, "CrashTestLateNullWrite"), 0) << r.out;
  }
  std::fprintf(stderr, "note: pc line named the library %d times, bare pc %d times\n", named, bare);
}
#endif

// ---- report header details, truncation, fault injection --------------------

// CH-49 (C-17): a null frame array with a non-zero count formats nothing.
TEST(CrashHandlerTest, FormatStackWithNullFramesFormatsNothing) {
  char buf[64];
  std::memset(buf, 'x', sizeof(buf));
  EXPECT_EQ(FormatStack(buf, sizeof(buf), nullptr, 3, true, true), 0u);
  EXPECT_STREQ(buf, "");
}

// The "tid N" field of the header line.
std::string HeaderTid(const std::string& header) {
  const size_t pos = header.find(", tid ");
  return pos == std::string::npos ? std::string() : header.substr(pos + std::strlen(", tid "));
}

// CH-50 (C-2): for kill() / raise() / abort() / sigqueue() si_addr holds the
// sender's pid and uid, not an address: the header names the sender and has
// no fault address. A real fault keeps it.
TEST(CrashHandlerTest, SentSignalNamesSenderInsteadOfFaultAddress) {
  struct Case {
    const char* name;
    int signo;
    void (*send)();
  };
  const Case cases[] = {
      {"abort", SIGABRT, [] { std::abort(); }},
      {"raise", SIGSEGV, [] { raise(SIGSEGV); }},
      {"kill", SIGBUS, [] { kill(getpid(), SIGBUS); }},
      {"sigqueue", SIGFPE, [] { sigqueue(getpid(), SIGFPE, sigval{}); }},
  };
  const std::string uid = std::to_string(getuid());
  for (const Case& c : cases) {
    ChildResult r = RunInChild(Options{}, [&c] {
      c.send();
      for (;;) pause();
    }, /*kill_after_sec=*/15);
    ASSERT_TRUE(WIFSIGNALED(r.status)) << c.name << "\n" << r.out;
    EXPECT_EQ(WTERMSIG(r.status), c.signo) << c.name << "\n" << r.out;
    const std::string header = HeaderLine(r.out, "*** IBMW crash: signal ");
    ASSERT_FALSE(header.empty()) << c.name << "\n" << r.out;
    EXPECT_EQ(header.find("fault addr"), std::string::npos) << c.name << ": " << header;
    // Single-threaded child: the sender pid is the reporting tid.
    const std::string tid = HeaderTid(header);
    ASSERT_FALSE(tid.empty()) << header;
#if defined(__OHOS__)
    // The HongMeng kernel delivers sigqueue() with si_pid 0.
    const std::string sender = std::strcmp(c.name, "sigqueue") == 0 ? "0" : tid;
#else
    const std::string& sender = tid;
#endif
    EXPECT_NE(header.find(", sent by pid " + sender + " uid " + uid + ", pc 0x"), std::string::npos)
        << c.name << ": " << header;
    EXPECT_EQ(r.out.find("*** caller"), std::string::npos) << c.name << "\n" << r.out;
    EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << c.name << "\n" << r.out;
  }

  ChildResult r = RunInChild(Options{}, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  const std::string header = HeaderLine(r.out, "*** IBMW crash: signal ");
  EXPECT_NE(header.find(", fault addr 0x0000000000000000, pc 0x"), std::string::npos) << header;
  EXPECT_EQ(header.find("sent by"), std::string::npos) << header;
}

#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
// CH-57: a seccomp trap (SIGSYS, si_code SYS_SECCOMP) names the address after
// the refused system call; si_addr means nothing there.
TEST(CrashHandlerTest, SeccompTrapShowsCallAddress) {
  ChildResult r = RunInChild(Options{}, [] {
#if defined(__x86_64__)
    constexpr unsigned kArch = AUDIT_ARCH_X86_64;
#else
    constexpr unsigned kArch = AUDIT_ARCH_AARCH64;
#endif
    // getppid() traps, everything else is allowed.
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(140);
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) _exit(141);
    syscall(SYS_getppid);
    _exit(142);
  }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << "exit " << (WIFEXITED(r.status) ? WEXITSTATUS(r.status) : -1) << "\n" << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSYS) << r.out;
  const std::string header = HeaderLine(r.out, "*** IBMW crash: signal ");
  EXPECT_EQ(header.rfind("*** IBMW crash: signal 31 (SIGSYS), code 1, call addr 0x", 0), 0u) << header;
  EXPECT_EQ(header.find("call addr 0x0000000000000000"), std::string::npos) << header;
  EXPECT_EQ(header.find("fault addr"), std::string::npos) << header;
  EXPECT_EQ(header.find("sent by"), std::string::npos) << header;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
}
#endif

// CH-51 (C-3): alt_stack_size = 0 on a re-Install() disables the alternate
// stack the previous Install() set up, and leaves someone else's alone.
TEST(CrashHandlerTest, ZeroAltStackSizeDisablesPreviousAltStack) {
  constexpr size_t kSize = 1024 * 1024;
  Options on;
  on.signals = {SIGUSR2};
  on.alt_stack_size = kSize;
  ChildResult r = RunInChild(on, [on] {
    stack_t ss;
    if (sigaltstack(nullptr, &ss) != 0 || (ss.ss_flags & SS_DISABLE) != 0 || ss.ss_size != kSize) _exit(120);
    Options off = on;
    off.alt_stack_size = 0;
    if (!Install(off)) _exit(121);
    if (sigaltstack(nullptr, &ss) != 0 || (ss.ss_flags & SS_DISABLE) == 0) _exit(122);

    static char other[256 * 1024];
    stack_t theirs;
    std::memset(&theirs, 0, sizeof(theirs));
    theirs.ss_sp = other;
    theirs.ss_size = sizeof(other);
    if (sigaltstack(&theirs, nullptr) != 0) _exit(123);
    if (!Install(off)) _exit(124);
    if (sigaltstack(nullptr, &ss) != 0 || (ss.ss_flags & SS_DISABLE) != 0 || ss.ss_sp != other) _exit(125);

    if (!Install(on)) _exit(126);
    if (sigaltstack(nullptr, &ss) != 0 || ss.ss_sp == other || ss.ss_size != kSize) _exit(127);
    Uninstall();
  });
  ASSERT_TRUE(WIFEXITED(r.status)) << r.out;
  EXPECT_EQ(WEXITSTATUS(r.status), 0) << r.out;
}

// CH-52 (C-4): a report longer than the 64 KiB buffer (128 frames with
// names of about 1.5 KB each) is cut, says so, and still ends with the
// "*** end of stacktrace" line, both on output_fd and in the callback.
// Each level must stay a separate call of this instantiation, named by
// dladdr, at every optimization level: GCC's IPA-SRA would otherwise turn it
// into a local ".isra" clone that dladdr cannot name, and the report would
// stay short. noipa (GCC) and optnone (Clang) keep it as written.
#if defined(__clang__)
#define IBMW_CH_KEEP_AS_WRITTEN __attribute__((noinline, optnone))
#else
#define IBMW_CH_KEEP_AS_WRITTEN __attribute__((noinline, noipa))
#endif
template <typename Seq>
IBMW_CH_KEEP_AS_WRITTEN int DeepTemplateFrames(int depth) {
  if (depth == 0) {
    CrashHere();
    return 0;
  }
  const int r = DeepTemplateFrames<Seq>(depth - 1);
  asm volatile("" ::: "memory");
  return r + 1;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TEST(CrashHandlerTest, LongReportIsTruncatedWithMarkerAndTrailer) {
  Options opt;
  opt.max_frames = kMaxFrames;
  opt.report_callback = &CallbackToFd;
  ChildResult r = RunInChild(opt, [] { (void)DeepTemplateFrames<std::make_integer_sequence<int, 300>>(200); },
                             /*kill_after_sec=*/30);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out.substr(0, 4096);
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV);
  EXPECT_EQ(r.out.find("faulted"), std::string::npos);
  const std::string marker = "\n*** IBMW crash: report truncated\n*** end of stacktrace (";

  // output_fd: the raw part fits, the symbolized part (mangled names) is cut.
  const size_t sym = r.out.find("*** symbolized stacktrace:\n");
  ASSERT_NE(sym, std::string::npos) << r.out.substr(0, 4096);
  EXPECT_EQ(r.out.rfind("report truncated", sym), std::string::npos) << "raw part was cut";
  EXPECT_NE(r.out.find(marker, sym), std::string::npos) << r.out.substr(r.out.size() - 512);
  EXPECT_TRUE(EndsWith(r.out, " frames)\n")) << r.out.substr(r.out.size() - 512);
  EXPECT_LT(r.out.size() - sym, 64u * 1024u);

  // The callback's copy is cut the same way (names this long do not fit the
  // demangling buffer and stay mangled).
  EXPECT_EQ(r.extra.rfind("*** IBMW crash: signal 11 (SIGSEGV)", 0), 0u) << r.extra.substr(0, 512);
  EXPECT_NE(r.extra.find(marker), std::string::npos) << r.extra.substr(r.extra.size() - 512);
  EXPECT_TRUE(EndsWith(r.extra, " frames)\n")) << r.extra.substr(r.extra.size() - 512);
  EXPECT_LT(r.extra.size(), 64u * 1024u);
  const size_t extra_sym = r.extra.find("*** symbolized stacktrace:\n");
  ASSERT_NE(extra_sym, std::string::npos) << r.extra.substr(0, 4096);
  EXPECT_NE(r.extra.find("DeepTemplateFrames", extra_sym), std::string::npos)
      << r.extra.substr(extra_sym, 4096);
}

// CH-55 (C-4): a report that fits has no truncation marker.
TEST(CrashHandlerTest, ShortReportHasNoTruncationMarker) {
  Options opt;
  opt.report_callback = &CallbackToFd;
  ChildResult r = RunInChild(opt, [] { CrashHere(); });
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(r.out.find("truncated"), std::string::npos) << r.out;
  EXPECT_EQ(r.extra.find("truncated"), std::string::npos) << r.extra;
  EXPECT_NE(r.extra.find("*** end of stacktrace"), std::string::npos) << r.extra;
}

long long MillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

void BlockWatchdogAndHang(const char*, size_t, void*) {
  sigset_t alarm_only;
  sigemptyset(&alarm_only);
  sigaddset(&alarm_only, SIGALRM);
  pthread_sigmask(SIG_BLOCK, &alarm_only, nullptr);
  for (;;) usleep(10 * 1000);
}

// CH-59 (round-2 review M1): the crash is on a worker thread while the main
// thread waits in join(), so the alarm lands on the main thread and is
// handed over to the reporting thread, whose callback blocks SIGALRM and
// hangs. The handed-over signal stays pending there; the re-armed alarm
// (one second later) ends the process on the main thread. Before the fix
// the process hung until killed.
TEST(CrashHandlerTest, WatchdogEndsReportWhoseCallbackBlocksSigalrm) {
  Options opt;
  opt.report_callback = &BlockWatchdogAndHang;
  opt.report_timeout_sec = 1;
  const auto start = std::chrono::steady_clock::now();
  ChildResult r = RunInChild(opt, [] {
    std::thread worker([] { CrashHere(); });
    worker.join();
  }, /*kill_after_sec=*/10);
  const long long elapsed_ms = MillisSince(start);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << "killed after the 10 s limit: the watchdog did not end it\n" << r.out;
  EXPECT_LT(elapsed_ms, 3000) << r.out;
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
}

#if !defined(IBMW_CH_NO_INTERPOSE)
// Fault injection for CH-53 / CH-54. The test program defines
// _Unwind_GetIPInfo and __cxa_demangle itself (below, after the namespace):
// the handler, linked into it statically, binds to them. They forward to the
// real functions (looked up with dlsym(RTLD_NEXT) during static
// initialization, never in a signal handler) unless a child sets g_inject
// before crashing. This needs the shared libgcc_s / libstdc++: with
// -static-libgcc, -static-libstdc++ or -static the test CMakeLists defines
// IBMW_CH_NO_INTERPOSE and these tests are not built.
enum Inject : int { kInjectNone, kInjectHangInWalk, kInjectFaultInDemangle };
volatile sig_atomic_t g_inject = kInjectNone;

using GetIPInfoFn = _Unwind_Ptr (*)(struct _Unwind_Context*, int*);
using DemangleFn = char* (*)(const char*, char*, size_t*, int*);
const GetIPInfoFn g_real_get_ip_info = reinterpret_cast<GetIPInfoFn>(dlsym(RTLD_NEXT, "_Unwind_GetIPInfo"));
const DemangleFn g_real_demangle = reinterpret_cast<DemangleFn>(dlsym(RTLD_NEXT, "__cxa_demangle"));

#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
// The child's alternate stack, shared with the parent so that it can read the
// interrupted context the kernel saved there after the child is gone (the
// state a core dump or a debugger would see).
struct SharedAltStack {
  uintptr_t ctx_offset;
  uintptr_t fault_pc;
  uintptr_t fault_sp;
  uintptr_t pc_during_walk;
  int recorded;
  alignas(64) char stack[1024 * 1024];
};
SharedAltStack* g_shared = nullptr;

const ucontext_t* SharedContext() {
  return reinterpret_cast<const ucontext_t*>(reinterpret_cast<const char*>(g_shared) + g_shared->ctx_offset);
}

void RecordIntoShared(int signo, siginfo_t* info, void* ctx) {
  if (g_shared->recorded == 0) {
    auto* uctx = static_cast<ucontext_t*>(ctx);
    g_shared->ctx_offset = static_cast<uintptr_t>(reinterpret_cast<char*>(uctx) - reinterpret_cast<char*>(g_shared));
    g_shared->fault_pc = CtxPc(uctx);
    g_shared->fault_sp = CtxSp(uctx);
    g_shared->recorded = 1;
  }
  g_inner_action.sa_sigaction(signo, info, ctx);
}

// Called by the injected hang inside the stack walk.
void HangInWalk() {
  g_shared->pc_during_walk = CtxPc(SharedContext());
  for (;;) usleep(10 * 1000);
}

// CH-53 (C-5): the watchdog fires while the walk runs from the caller (pc/sp
// moved, see CH-43): it puts the faulting registers back before ending the
// process, like the other ways out.
TEST(CrashHandlerTest, WatchdogRestoresInterruptedContext) {
  ASSERT_NE(g_real_get_ip_info, nullptr) << "dlsym(RTLD_NEXT, \"_Unwind_GetIPInfo\") failed: no fault injection";
  void* mem = mmap(nullptr, sizeof(SharedAltStack), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mem, MAP_FAILED);
  g_shared = new (mem) SharedAltStack();
  Options opt;
  opt.alt_stack_size = 0;
  opt.report_timeout_sec = 1;
  ChildResult r = RunInChild(opt, [] {
    stack_t ss;
    std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_shared->stack;
    ss.ss_size = sizeof(g_shared->stack);
    if (sigaltstack(&ss, nullptr) != 0) _exit(130);
    if (sigaction(SIGSEGV, nullptr, &g_inner_action) != 0) _exit(131);
    struct sigaction wrapper = g_inner_action;
    wrapper.sa_sigaction = &RecordIntoShared;
    if (sigaction(SIGSEGV, &wrapper, nullptr) != 0) _exit(132);
    g_inject = kInjectHangInWalk;
    CallThroughPointer(&g_wild_fn);
  }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_NE(r.out.find("*** caller 0x"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: report timed out"), std::string::npos)
      << "the walk did not hang: the handler did not call the test's _Unwind_GetIPInfo\n" << r.out;
  EXPECT_EQ(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  ASSERT_EQ(g_shared->recorded, 1);
  EXPECT_EQ(g_shared->fault_pc, 0x1234u);
  // The hang was inside the moved window ...
  EXPECT_NE(g_shared->pc_during_walk, 0x1234u);
  EXPECT_NE(g_shared->pc_during_walk, 0u);
  // ... and the context the process died with is the fault site again.
  EXPECT_EQ(CtxPc(SharedContext()), g_shared->fault_pc);
  EXPECT_EQ(CtxSp(SharedContext()), g_shared->fault_sp);
  munmap(mem, sizeof(SharedAltStack));
  g_shared = nullptr;
}

// CH-58 (round-2 review M3): as CH-53, but the crash is on a worker thread
// while the main thread waits in join(). The alarm lands on the main thread
// (the kernel prefers it for a process-directed signal it does not block),
// which hands it over to the reporting thread: that thread writes "report
// timed out", puts the faulting registers back and ends the process, about
// one second after the crash (the last-resort exit, one second later, would
// not write the message).
TEST(CrashHandlerTest, WatchdogHandedOverFromJoiningThreadRestoresContext) {
  ASSERT_NE(g_real_get_ip_info, nullptr) << "dlsym(RTLD_NEXT, \"_Unwind_GetIPInfo\") failed: no fault injection";
  void* mem = mmap(nullptr, sizeof(SharedAltStack), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mem, MAP_FAILED);
  g_shared = new (mem) SharedAltStack();
  Options opt;
  opt.alt_stack_size = 0;
  opt.report_timeout_sec = 1;
  const auto start = std::chrono::steady_clock::now();
  ChildResult r = RunInChild(opt, [] {
    if (sigaction(SIGSEGV, nullptr, &g_inner_action) != 0) _exit(131);
    struct sigaction wrapper = g_inner_action;
    wrapper.sa_sigaction = &RecordIntoShared;
    if (sigaction(SIGSEGV, &wrapper, nullptr) != 0) _exit(132);
    std::thread worker([] {
      stack_t ss;
      std::memset(&ss, 0, sizeof(ss));
      ss.ss_sp = g_shared->stack;
      ss.ss_size = sizeof(g_shared->stack);
      if (sigaltstack(&ss, nullptr) != 0) _exit(130);
      g_inject = kInjectHangInWalk;
      CallThroughPointer(&g_wild_fn);
    });
    worker.join();
  }, /*kill_after_sec=*/15);
  const long long elapsed_ms = MillisSince(start);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  EXPECT_LT(elapsed_ms, 2000) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: report timed out"), std::string::npos)
      << "the walk did not hang, or the reporting thread did not take the handed-over signal\n" << r.out;
  ASSERT_EQ(g_shared->recorded, 1);
  EXPECT_EQ(g_shared->fault_pc, 0x1234u);
  EXPECT_NE(g_shared->pc_during_walk, 0x1234u);
  EXPECT_NE(g_shared->pc_during_walk, 0u);
  EXPECT_EQ(CtxPc(SharedContext()), g_shared->fault_pc);
  EXPECT_EQ(CtxSp(SharedContext()), g_shared->fault_sp);
  munmap(mem, sizeof(SharedAltStack));
  g_shared = nullptr;
}
#endif

// CH-54 (C-6): a fault while the callback's copy is demangled is reported as
// a symbol lookup fault, not as a callback fault; the callback never ran.
TEST(CrashHandlerTest, FaultWhileDemanglingForCallbackIsSymbolLookupFault) {
  ASSERT_NE(g_real_demangle, nullptr) << "dlsym(RTLD_NEXT, \"__cxa_demangle\") failed: no fault injection";
  Options opt;
  opt.report_callback = &CallbackToFd;
  ChildResult r = RunInChild(opt, [] {
    g_inject = kInjectFaultInDemangle;
    CrashHere();
  }, /*kill_after_sec=*/15);
  ASSERT_TRUE(WIFSIGNALED(r.status)) << r.out;
  EXPECT_EQ(WTERMSIG(r.status), SIGSEGV) << r.out;
  // output_fd does not demangle: its report is complete.
  EXPECT_NE(r.out.find("*** end of stacktrace"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("*** IBMW crash: symbol lookup faulted"), std::string::npos)
      << "no fault: the handler did not call the test's __cxa_demangle\n" << r.out;
  EXPECT_EQ(r.out.find("report callback faulted"), std::string::npos) << r.out;
  EXPECT_TRUE(r.extra.empty()) << r.extra;
}
#endif  // !IBMW_CH_NO_INTERPOSE

}  // namespace crash_handler_test

#if !defined(IBMW_CH_NO_INTERPOSE)
// The fault injection points of CH-53 / CH-54 (see g_inject). A call before
// the static initialization above (an exception thrown by another static
// constructor) looks the function up itself; none happens in a signal
// handler.
extern "C" _Unwind_Ptr _Unwind_GetIPInfo(struct _Unwind_Context* ctx, int* ip_before_insn) {
  using crash_handler_test::GetIPInfoFn;
  GetIPInfoFn real = crash_handler_test::g_real_get_ip_info;
  if (real == nullptr) real = reinterpret_cast<GetIPInfoFn>(dlsym(RTLD_NEXT, "_Unwind_GetIPInfo"));
#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)
  if (crash_handler_test::g_inject == crash_handler_test::kInjectHangInWalk) crash_handler_test::HangInWalk();
#endif
  return real(ctx, ip_before_insn);
}

extern "C" char* __cxa_demangle(const char* mangled, char* out, size_t* len, int* status) {
  using crash_handler_test::DemangleFn;
  DemangleFn real = crash_handler_test::g_real_demangle;
  if (real == nullptr) real = reinterpret_cast<DemangleFn>(dlsym(RTLD_NEXT, "__cxa_demangle"));
  if (crash_handler_test::g_inject == crash_handler_test::kInjectFaultInDemangle) crash_handler_test::CrashHere();
  return real(mangled, out, len, status);
}
#endif  // !IBMW_CH_NO_INTERPOSE
