// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <csignal>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>  // siginfo_t

#include "base/ibmw_config.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::logger {

class CrashSignalHandling {
 public:
  CrashSignalHandling();
  // Puts back the handlers found at construction (where they are still ours)
  // and detaches the logger: a crash afterwards no longer reaches objects
  // whose owner is gone.
  ~CrashSignalHandling();

  CrashSignalHandling(const CrashSignalHandling&) = delete;
  CrashSignalHandling& operator=(const CrashSignalHandling&) = delete;

  // The signal handler reads the logger without a lock, so the pointer it
  // reads is switched before the previous owner is released.
  static void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) {
    logger_.store(logger_ptr ? logger_ptr.get() : &default_logger_, std::memory_order_release);
    logger_owner_ = logger_ptr;
  }
  static const ibmw::common::util::LoggerWrapper& GetLogger() {
    return *logger_.load(std::memory_order_acquire);
  }

  // Called by the handler right after it logged the report, before the
  // process ends: the log drivers write asynchronously, so fn(ctx) waits
  // (bounded, see LoggingSubsystem::FlushForCrash) until the report is
  // written. Set before crashes can happen.
  //
  // The hook is one atomic pointer to an immutable {fn, ctx} record owned by
  // the caller, so a crash sees a whole record or none, never fn of one owner
  // with ctx of another. SetCrashFlush() publishes the record (nullptr: no
  // hook; the last call wins). ClearCrashFlush() unpublishes it only if it is
  // still the published one, then waits until no crash handler is running
  // the hook any more: once it returns, the owner may destroy the record and
  // ctx. Clearing removes the hook; it does not hand it back to an owner that
  // published earlier and was replaced (that owner has no hook until it
  // publishes again). Not for signal context.
  using CrashFlushFunc = void (*)(void* ctx);
  struct CrashFlushHook {
    CrashFlushFunc fn;
    void* ctx;
  };
  static void SetCrashFlush(const CrashFlushHook* hook) { crash_flush_hook_.store(hook); }
  // The published record, or nullptr.
  static const CrashFlushHook* GetCrashFlush() { return crash_flush_hook_.load(); }
  static void ClearCrashFlush(const CrashFlushHook* hook) {
    const CrashFlushHook* expected = hook;
    crash_flush_hook_.compare_exchange_strong(expected, nullptr);
    // Also after a failed exchange: a handler may have loaded the record
    // before another owner replaced it. Sequentially consistent with the
    // increment in RunCrashFlush(): a handler that registers after this load
    // reads the exchanged pointer, never this record.
    while (crash_flush_readers_.load() != 0) std::this_thread::yield();
  }

  // Re-records the module table of the selfdev crash handler, so a crash in
  // a library dlopen'ed since Install() (packages, extensions) is reported
  // with module+offset instead of a bare pc. Call after loading libraries,
  // never from a signal handler. No-op for the other handler variants.
  static void RefreshModules();

 private:
#if defined(IBMW_SELFDEV_CRASH_HANDLER)
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
  inline static const ibmw::common::util::LoggerWrapper default_logger_{};
  inline static std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_owner_;
  inline static std::atomic<const ibmw::common::util::LoggerWrapper*> logger_{&default_logger_};
  // Runs the hook from signal context: lock-free atomics, no allocation.
  static void RunCrashFlush() {
    crash_flush_readers_.fetch_add(1);
    const CrashFlushHook* const hook = crash_flush_hook_.load();
    if (hook != nullptr) hook->fn(hook->ctx);
    crash_flush_readers_.fetch_sub(1);
  }

  static_assert(std::atomic<const CrashFlushHook*>::is_always_lock_free);
  static_assert(std::atomic<int>::is_always_lock_free);
  inline static std::atomic<const CrashFlushHook*> crash_flush_hook_{nullptr};
  // Crash handlers between loading the hook and returning from it.
  inline static std::atomic<int> crash_flush_readers_{0};
#if defined(IBMW_SELFDEV_CRASH_HANDLER)
  // The selfdev handler is process-wide: the last instance uninstalls it.
  inline static std::atomic<int> selfdev_instances_{0};
#else
  std::unique_ptr<char, FreeDeleter> alt_stack_{nullptr};
  stack_t previous_alt_stack_{};
  std::thread::id alt_stack_thread_;
  std::vector<std::pair<int, struct sigaction>> previous_actions_;
#endif
};

}  // namespace ibmw::runtime::core::logger
