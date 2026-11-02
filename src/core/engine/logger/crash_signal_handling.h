// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <csignal>
#include <memory>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <signal.h>  // siginfo_t
#endif

#include "base/ibmw_config.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::logger {

class CrashSignalHandling {
 public:
  CrashSignalHandling();
  ~CrashSignalHandling() = default;

  CrashSignalHandling(const CrashSignalHandling&) = delete;
  CrashSignalHandling& operator=(const CrashSignalHandling&) = delete;

  static void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  static const ibmw::common::util::LoggerWrapper& GetLogger() { return *logger_ptr_; }

  // Called by the handler right after it logged the report, before the
  // process ends: the log drivers write asynchronously, so fn(ctx) waits
  // (bounded, see LoggingSubsystem::FlushForCrash) until the report is
  // written. Set before crashes can happen. Stored in two atomics (trivially
  // destructible, lock-free): a crash during ClearCrashFlush() or static
  // destruction sees either the hook or none, never a destroyed object.
  using CrashFlushFunc = void (*)(void* ctx);
  static void SetCrashFlush(CrashFlushFunc fn, void* ctx) {
    crash_flush_ctx_.store(ctx, std::memory_order_release);
    crash_flush_fn_.store(fn, std::memory_order_release);
  }
  // Removes the hook if ctx set it (another owner's hook is left alone).
  static void ClearCrashFlush(void* ctx) {
    CrashFlushFunc fn = crash_flush_fn_.load(std::memory_order_acquire);
    if (crash_flush_ctx_.load(std::memory_order_acquire) != ctx) return;
    crash_flush_fn_.compare_exchange_strong(fn, nullptr, std::memory_order_acq_rel);
  }

  // Re-records the module table of the selfdev crash handler, so a crash in
  // a library dlopen'ed since Install() (packages, extensions) is reported
  // with module+offset instead of a bare pc. Call after loading libraries,
  // never from a signal handler. No-op for the other handler variants.
  static void RefreshModules();

 private:
#if defined(_WIN32)
  static void PrintStacktrace(CONTEXT* ctx, int skip_frames);
  static LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* info);
  static void AbortHandler(int);
#elif defined(IBMW_SELFDEV_CRASH_HANDLER)
  // Receives the demangled report from ibmw::selfdev::crash and logs it.
  static void ReportToLog(const char* report, size_t len, void* user_data);
#else
  struct FreeDeleter {
    void operator()(char* p) const noexcept;
  };

  static void HandleSignal(int signo, siginfo_t* info, void* _ctx);
  static void SigHandler(int signo, siginfo_t* info, void* _ctx);
  void EnsureAltStack();
  void InstallHandlers(const std::vector<int>& signals);
#endif

 private:
  inline static std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_ =
      std::make_shared<ibmw::common::util::LoggerWrapper>();
  // Runs the hook from signal context: loads both atomics, no allocation.
  static void RunCrashFlush() {
    const CrashFlushFunc fn = crash_flush_fn_.load(std::memory_order_acquire);
    void* const ctx = crash_flush_ctx_.load(std::memory_order_acquire);
    if (fn != nullptr) fn(ctx);
  }

  inline static std::atomic<CrashFlushFunc> crash_flush_fn_{nullptr};
  inline static std::atomic<void*> crash_flush_ctx_{nullptr};
#if !defined(_WIN32) && !defined(IBMW_SELFDEV_CRASH_HANDLER)
  std::unique_ptr<char, FreeDeleter> alt_stack_{nullptr};
#endif
#if !defined(_WIN32)
  bool loaded_{false};
#endif
};

}  // namespace ibmw::runtime::core::logger
