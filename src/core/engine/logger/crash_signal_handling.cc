// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/logger/crash_signal_handling.h"

#include "base/log_util.h"

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
#include "backward.hpp"
#endif

#if defined(_WIN32)
  #include <windows.h>
  #include <cstdint>
  #include <cstdlib>
  #include <sstream>

#else
  #include <signal.h>
  #include <ucontext.h>
  #include <unistd.h>
  #include <cstring>
#endif

namespace ibmw::runtime::core::logger {

#if defined(_WIN32) && defined(IBMW_SELFDEV_CRASH_HANDLER)

// Addresses only: symbolize offline from the module map. Not verified on
// Windows yet (see st/selfdev_process/crash_handler/功能设计.md).
void CrashSignalHandling::PrintStacktrace(CONTEXT* ctx, int skip_frames) {
  (void)ctx;
  void* frames[ibmw::selfdev::crash::kMaxFrames];
  size_t count = ibmw::selfdev::crash::CaptureStack(frames, 64, static_cast<size_t>(skip_frames) + 1);
  std::string out(count * 32 + 1, '\0');
  out.resize(ibmw::selfdev::crash::FormatStack(out.data(), out.size(), frames, count, false, false));
  MW_ERROR("CRASH SIGNAL HANDLING STACKTRACE:\n{}", out);
}

LONG WINAPI CrashSignalHandling::UnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    MW_ERROR("CRASH SIGNAL HANDLING EXCEPTION CODE: 0x{:08X}", static_cast<uint32_t>(info->ExceptionRecord->ExceptionCode));
  } else {
    MW_ERROR("CRASH SIGNAL HANDLING: unhandled exception (no exception record)");
  }
  PrintStacktrace(info ? info->ContextRecord : nullptr, 0);
  return EXCEPTION_CONTINUE_SEARCH;
}

void CrashSignalHandling::AbortHandler(int) {
  MW_ERROR("CRASH SIGNAL HANDLING: SIGABRT");
  PrintStacktrace(nullptr, 0);
  std::abort();
}

CrashSignalHandling::CrashSignalHandling() {
  ::SetUnhandledExceptionFilter(&UnhandledExceptionFilter);
  std::signal(SIGABRT, &AbortHandler);
}

#elif defined(_WIN32)

void CrashSignalHandling::PrintStacktrace(CONTEXT* ctx, int skip_frames) {
  backward::Printer printer;

  backward::StackTrace st;
  st.set_machine_type(printer.resolver().machine_type());

  HANDLE real_thd = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                  GetCurrentProcess(), &real_thd, 0, FALSE,
                  DUPLICATE_SAME_ACCESS);
  if (real_thd) st.set_thread_handle(real_thd);

  st.load_here(32 + skip_frames, ctx);
  st.skip_n_firsts(skip_frames);

  printer.address = true;
  std::stringstream ss;
  printer.print(st, ss);

  MW_ERROR("CRASH SIGNAL HANDLING STACKTRACE:\n{}", ss.str());

  if (real_thd) CloseHandle(real_thd);
}

LONG WINAPI CrashSignalHandling::UnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    MW_ERROR("CRASH SIGNAL HANDLING EXCEPTION CODE: 0x{:08X}", static_cast<uint32_t>(info->ExceptionRecord->ExceptionCode));
  } else {
    MW_ERROR("CRASH SIGNAL HANDLING: unhandled exception (no exception record)");
  }

  if (info && info->ContextRecord) {
    PrintStacktrace(info->ContextRecord, 0);
  }

  // Let other handlers / default Windows error reporting continue.
  return EXCEPTION_CONTINUE_SEARCH;
}

void CrashSignalHandling::AbortHandler(int) {
  CONTEXT ctx;
  RtlCaptureContext(&ctx);
  MW_ERROR("CRASH SIGNAL HANDLING: SIGABRT");
  PrintStacktrace(&ctx, 0);
  std::abort();
}

CrashSignalHandling::CrashSignalHandling() {
  ::SetUnhandledExceptionFilter(&UnhandledExceptionFilter);
  std::signal(SIGABRT, &AbortHandler);
}

#elif defined(IBMW_SELFDEV_CRASH_HANDLER)

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
  loaded_ = ibmw::selfdev::crash::Install(options);
  #else
  loaded_ = false;
  #endif
}

#elif IBMW_CRASH_SIGNAL_HANDLING_HAS_BACKWARD

std::vector<int> DefaultSignals() {
  return backward::SignalHandling::make_default_signals();
}

void CrashSignalHandling::FreeDeleter::operator()(char* p) const noexcept {
  // [Intentional Leak]
  // This memory is registered via sigaltstack() as the alternative signal stack.
  // The kernel retains this address independent of this object's lifecycle.
  //
  // Freeing this memory poses a critical risk: if a signal occurs after destruction,
  // the kernel will write to the freed memory, causing heap corruption.
  // This memory must remain valid for the entire process lifetime.
}

CrashSignalHandling::CrashSignalHandling() { InstallHandlers(DefaultSignals()); }

void CrashSignalHandling::HandleSignal(int, siginfo_t* info, void* _ctx) {
  ucontext_t* uctx = static_cast<ucontext_t*>(_ctx);

  backward::StackTrace st;
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

  #if _XOPEN_SOURCE >= 700 || _POSIX_C_SOURCE >= 200809L
  psiginfo(info, nullptr);
  #else
  (void)info;
  #endif
}

  #ifdef __GNUC__
__attribute__((noreturn))
  #endif
void
CrashSignalHandling::SigHandler(int signo, siginfo_t* info, void* _ctx) {
  // Async-signal-safe part first: the crash is reported even if the
  // symbolized trace below never completes.
  char msg[] = "IBMW crash signal: 00\n";
  msg[sizeof(msg) - 4] = static_cast<char>('0' + (signo / 10) % 10);
  msg[sizeof(msg) - 3] = static_cast<char>('0' + signo % 10);
  (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);

  // HandleSignal() allocates, symbolizes and logs, none of which is
  // async-signal-safe: if the crash happened while malloc or the log lock was
  // held it would hang forever. Bound it with a watchdog (SIGALRM is left
  // unblocked by InstallHandlers and terminates by default).
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof dfl);
  dfl.sa_handler = SIG_DFL;
  (void)sigaction(SIGALRM, &dfl, nullptr);
  alarm(10);

  HandleSignal(signo, info, _ctx);

  // Forward the signal (SA_RESETHAND makes it use default on re-raise).
  raise(info->si_signo);
  _exit(EXIT_FAILURE);
}

void CrashSignalHandling::EnsureAltStack() {
  const size_t stack_size = 1024 * 1024 * 8;
  alt_stack_.reset(static_cast<char*>(std::malloc(stack_size)));
  if (!alt_stack_) {
    loaded_ = false;
    return;
  }

  stack_t ss;
  ss.ss_sp = alt_stack_.get();
  ss.ss_size = stack_size;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, nullptr) < 0) {
    loaded_ = false;
  }
}

void CrashSignalHandling::InstallHandlers(const std::vector<int>& signals) {
  loaded_ = true;
  EnsureAltStack();

  for (int sig : signals) {
    struct sigaction action;
    std::memset(&action, 0, sizeof action);
    action.sa_flags = static_cast<int>(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND);
    sigfillset(&action.sa_mask);
    sigdelset(&action.sa_mask, sig);
    sigdelset(&action.sa_mask, SIGALRM);  // crash-report watchdog, see SigHandler
    action.sa_sigaction = &SigHandler;

    if (sigaction(sig, &action, nullptr) < 0) {
      loaded_ = false;
    }
  }
}

#else

std::vector<int> DefaultSignals() {
  return {};
}

void CrashSignalHandling::FreeDeleter::operator()(char* p) const noexcept {
  (void)p;
}

CrashSignalHandling::CrashSignalHandling() { loaded_ = false; }

void CrashSignalHandling::HandleSignal(int, siginfo_t*, void*) {}

void CrashSignalHandling::SigHandler(int, siginfo_t*, void*) {}

void CrashSignalHandling::EnsureAltStack() { loaded_ = false; }

void CrashSignalHandling::InstallHandlers(const std::vector<int>&) { loaded_ = false; }

#endif

void CrashSignalHandling::RefreshModules() {
#if defined(IBMW_SELFDEV_CRASH_HANDLER) && !defined(_WIN32) && IBMW_CRASH_SIGNAL_HANDLING_ENABLED
  ibmw::selfdev::crash::RefreshModules();
#endif
}

}  // namespace ibmw::runtime::core::logger
