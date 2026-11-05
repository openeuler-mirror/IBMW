// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/logger/crash_signal_handling.h"

#include "base/log_util.h"
#include "engine/thread_tools.h"

// Handlers are not installed on OHOS until the unwinder is validated on
// target boards (same gate for backward-cpp and the selfdev handler).
#if !defined(__OHOS__) && !defined(__OHOS)
  #define IBMW_CRASH_SIGNAL_HANDLING_ENABLED 1
#else
  #define IBMW_CRASH_SIGNAL_HANDLING_ENABLED 0
#endif

#if defined(IBMW_SELFDEV_CRASH_HANDLER)
  #define IBMW_CRASH_SIGNAL_HANDLING_HAS_BACKWARD 0
  #include <string_view>

  #include "ibmw_selfdev/crash_handler/crash_handler.h"
#else
  #define IBMW_CRASH_SIGNAL_HANDLING_HAS_BACKWARD IBMW_CRASH_SIGNAL_HANDLING_ENABLED
#endif

#if IBMW_CRASH_SIGNAL_HANDLING_HAS_BACKWARD
#include <execinfo.h>

#include "backward.hpp"
#endif

#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ibmw::runtime::core::logger {

#if defined(IBMW_SELFDEV_CRASH_HANDLER)

void CrashSignalHandling::ReportToLog(const char* report, size_t len, void*) {
  MW_ERROR("Crash signal handling stacktrace: \n{}", std::string_view(report, len));
  // The handler ends the process as soon as this returns.
  RunCrashFlush();
}

CrashSignalHandling::CrashSignalHandling() {
  #if IBMW_CRASH_SIGNAL_HANDLING_ENABLED
  ibmw::selfdev::crash::Options options;
  options.alt_stack_size = 1024 * 1024 * 8;  // same as the backward-cpp path
  options.report_callback = &ReportToLog;
  // The raw report also goes to stderr (async-signal-safe) in case logging
  // itself is what crashed.
  options.output_fd = STDERR_FILENO;
  selfdev_instances_.fetch_add(1);
  (void)ibmw::selfdev::crash::Install(options);
  #endif
}

CrashSignalHandling::~CrashSignalHandling() {
  #if IBMW_CRASH_SIGNAL_HANDLING_ENABLED
  // Restores the actions from before Install() where the handler is still
  // ours, so no crash reports through a logger that is gone.
  if (selfdev_instances_.fetch_sub(1) == 1) ibmw::selfdev::crash::Uninstall();
  #endif
  SetLogger(nullptr);
}

#elif IBMW_CRASH_SIGNAL_HANDLING_HAS_BACKWARD

std::vector<int> DefaultSignals() {
  return backward::SignalHandling::make_default_signals();
}

void CrashSignalHandling::FreeDeleter::operator()(char* p) const noexcept { std::free(p); }

CrashSignalHandling::CrashSignalHandling() {
  InstallHandlers(DefaultSignals());
  util::EnableAltSignalStacks(true);  // executor threads started from now on
}

CrashSignalHandling::~CrashSignalHandling() {
  util::EnableAltSignalStacks(false);
  for (auto it = previous_actions_.rbegin(); it != previous_actions_.rend(); ++it) {
    struct sigaction current;
    // SA_RESETHAND or a later installer may have replaced ours: keep that.
    if (sigaction(it->first, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) != 0 &&
        current.sa_sigaction == &SigHandler) {
      (void)sigaction(it->first, &it->second, nullptr);
    }
  }
  SetLogger(nullptr);

  if (!alt_stack_) return;
  // The alternate stack belongs to the installing thread. Free it only when
  // that thread can switch back to its previous one; otherwise the kernel
  // may still use it, so it stays allocated for the process lifetime.
  stack_t current{};
  if (std::this_thread::get_id() != alt_stack_thread_ || sigaltstack(nullptr, &current) != 0 ||
      current.ss_sp != alt_stack_.get() || sigaltstack(&previous_alt_stack_, nullptr) != 0) {
    (void)alt_stack_.release();
  }
}

namespace {
// Program counter at the fault, or nullptr where it is not known.
void* FaultingPc(void* _ctx) {
  ucontext_t* uctx = static_cast<ucontext_t*>(_ctx);
  void* error_addr = nullptr;

  #ifdef REG_RIP          // x86_64
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.gregs[REG_RIP]);
  #elif defined(REG_EIP)  // x86_32
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.gregs[REG_EIP]);
  #elif defined(__arm__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.arm_pc);
  #elif defined(__aarch64__)
    #if defined(__APPLE__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext->__ss.__pc);
    #else
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.pc);
    #endif
  #elif defined(__mips__)
  error_addr = reinterpret_cast<void*>(
      reinterpret_cast<struct sigcontext*>(&uctx->uc_mcontext)->sc_pc);
  #elif defined(__ppc__) || defined(__powerpc) || defined(__powerpc__) || defined(__POWERPC__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.regs->nip);
  #elif defined(__riscv)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.__gregs[REG_PC]);
  #elif defined(__s390x__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext.psw.addr);
  #elif defined(__APPLE__) && defined(__x86_64__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext->__ss.__rip);
  #elif defined(__APPLE__)
  error_addr = reinterpret_cast<void*>(uctx->uc_mcontext->__ss.__eip);
  #endif

  (void)uctx;
  return error_addr;
}
}  // namespace

void CrashSignalHandling::HandleSignal(int, siginfo_t* info, void* _ctx) {
  ucontext_t* uctx = static_cast<ucontext_t*>(_ctx);

  backward::StackTrace st;
  void* error_addr = FaultingPc(_ctx);

  if (error_addr) {
    st.load_from(error_addr, 32, reinterpret_cast<void*>(uctx), info->si_addr);
  } else {
    st.load_here(32, reinterpret_cast<void*>(uctx), info->si_addr);
  }
  backward::Printer printer;
  printer.address = true;
  std::stringstream ss;
  printer.print(st, ss);
  MW_ERROR("Crash signal handling signal: {}, stacktrace: \n{}", info->si_signo, ss.str());
  // SigHandler re-raises the signal right after this returns.
  RunCrashFlush();
}

namespace {
volatile sig_atomic_t g_crash_signo = 0;

void WriteStderr(const char* text, size_t size) noexcept {
  while (size > 0) {
    const ssize_t written = write(STDERR_FILENO, text, size);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    text += written;
    size -= static_cast<size_t>(written);
  }
}

// Appends "0x" and `value` in hex to `out`; returns the end.
char* AppendHex(char* out, uintptr_t value) noexcept {
  *out++ = '0';
  *out++ = 'x';
  int shift = static_cast<int>(sizeof(value) * 8) - 4;
  while (shift > 0 && ((value >> shift) & 0xf) == 0) shift -= 4;
  for (; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 0xf];
  return out;
}

// Where it crashed (async-signal-safe; `addr` is the fault address only when
// `code` is positive, i.e. sent by the kernel) and a raw trace, one
// "binary(symbol+offset)[address]" line per frame. backtrace() is not
// async-signal-safe by the letter, only in practice once preloaded, so this
// runs under the watchdog too.
void WriteRawCrashReport(const siginfo_t* info, void* _ctx) noexcept {
  char line[96];
  char* end = line;
  for (const char* text = "IBMW crash pc: "; *text != '\0';) *end++ = *text++;
  end = AppendHex(end, reinterpret_cast<uintptr_t>(FaultingPc(_ctx)));
  for (const char* text = " addr: "; *text != '\0';) *end++ = *text++;
  end = AppendHex(end, reinterpret_cast<uintptr_t>(info->si_addr));
  for (const char* text = " code: "; *text != '\0';) *end++ = *text++;
  end = AppendHex(end, static_cast<uintptr_t>(static_cast<unsigned>(info->si_code)));
  *end++ = '\n';
  WriteStderr(line, static_cast<size_t>(end - line));

  void* frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

// Watchdog for a hung crash report: terminate with the original signal (now
// SIG_DFL thanks to SA_RESETHAND) so the core dump and exit status are kept.
void CrashReportTimeout(int) {
  const int signo = g_crash_signo;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, signo);
  (void)sigprocmask(SIG_UNBLOCK, &set, nullptr);
  raise(signo);
  _exit(EXIT_FAILURE);
}
}  // namespace

  #ifdef __GNUC__
__attribute__((noreturn))
  #endif
void
CrashSignalHandling::SigHandler(int signo, siginfo_t* info, void* _ctx) {
  // Nothing below may hang the process: even a write to a stderr pipe nobody
  // reads blocks. Bound it all with a watchdog (SIGALRM is left unblocked by
  // InstallHandlers).
  g_crash_signo = signo;
  struct sigaction on_timeout;
  std::memset(&on_timeout, 0, sizeof on_timeout);
  on_timeout.sa_handler = &CrashReportTimeout;
  sigemptyset(&on_timeout.sa_mask);
  (void)sigaction(SIGALRM, &on_timeout, nullptr);
  alarm(10);

  // The raw part first: the crash is reported even if the symbolized trace
  // below never completes.
  char msg[] = "IBMW crash signal: 00\n";
  msg[sizeof(msg) - 4] = static_cast<char>('0' + (signo / 10) % 10);
  msg[sizeof(msg) - 3] = static_cast<char>('0' + signo % 10);
  WriteStderr(msg, sizeof(msg) - 1);
  WriteRawCrashReport(info, _ctx);

  // HandleSignal() allocates, symbolizes and logs, none of which is
  // async-signal-safe: if the crash happened while malloc or the log lock was
  // held it would hang. It gets a full watchdog period of its own.
  alarm(10);
  HandleSignal(signo, info, _ctx);

  // Forward the signal (SA_RESETHAND makes it use default on re-raise).
  raise(info->si_signo);
  _exit(EXIT_FAILURE);
}

void CrashSignalHandling::EnsureAltStack() {
  const size_t stack_size = util::kCrashAltStackSize;
  alt_stack_.reset(static_cast<char*>(std::malloc(stack_size)));
  if (!alt_stack_) {
    return;
  }

  stack_t ss;
  ss.ss_sp = alt_stack_.get();
  ss.ss_size = stack_size;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, &previous_alt_stack_) < 0) {
    alt_stack_.reset();  // never installed
    return;
  }
  alt_stack_thread_ = std::this_thread::get_id();
}

void CrashSignalHandling::InstallHandlers(const std::vector<int>& signals) {
  EnsureAltStack();
  // The first backtrace() loads libgcc_s, which allocates: do it here, not in
  // the handler.
  void* frame = nullptr;
  (void)backtrace(&frame, 1);

  for (int sig : signals) {
    struct sigaction action;
    std::memset(&action, 0, sizeof action);
    action.sa_flags = static_cast<int>(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND);
    sigfillset(&action.sa_mask);
    sigdelset(&action.sa_mask, sig);
    sigdelset(&action.sa_mask, SIGALRM);  // crash-report watchdog, see SigHandler
    action.sa_sigaction = &SigHandler;

    struct sigaction previous;
    if (sigaction(sig, &action, &previous) < 0) {
      continue;
    }
    previous_actions_.emplace_back(sig, previous);
  }
}

#else

std::vector<int> DefaultSignals() {
  return {};
}

void CrashSignalHandling::FreeDeleter::operator()(char* p) const noexcept {
  (void)p;
}

CrashSignalHandling::CrashSignalHandling() = default;

CrashSignalHandling::~CrashSignalHandling() { SetLogger(nullptr); }

void CrashSignalHandling::HandleSignal(int, siginfo_t*, void*) {}

void CrashSignalHandling::SigHandler(int, siginfo_t*, void*) {}

void CrashSignalHandling::EnsureAltStack() {}

void CrashSignalHandling::InstallHandlers(const std::vector<int>&) {}

#endif

void CrashSignalHandling::RefreshModules() {
#if defined(IBMW_SELFDEV_CRASH_HANDLER) && IBMW_CRASH_SIGNAL_HANDLING_ENABLED
  ibmw::selfdev::crash::RefreshModules();
#endif
}

}  // namespace ibmw::runtime::core::logger
