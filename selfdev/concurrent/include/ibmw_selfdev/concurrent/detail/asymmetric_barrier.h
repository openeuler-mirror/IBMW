// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define IBMW_SELFDEV_HAS_MEMBARRIER 0
#if defined(__linux__) && !defined(IBMW_SELFDEV_NO_MEMBARRIER) && defined(__has_include)
  #if __has_include(<linux/membarrier.h>)
    #include <linux/membarrier.h>
    #include <sys/syscall.h>
    #include <unistd.h>
    // MEMBARRIER_CMD_* are enumerators; only the syscall number is testable.
    #if defined(SYS_membarrier)
      #undef IBMW_SELFDEV_HAS_MEMBARRIER
      #define IBMW_SELFDEV_HAS_MEMBARRIER 1
    #endif
  #endif
#endif

// IBMW_SELFDEV_NO_MEMBARRIER must be the same in every translation unit:
// set it through the CMake option IBMW_SELFDEV_CONCURRENT_NO_MEMBARRIER or
// globally. Units that disagree can pair a compiler-only light side with a
// fence-only heavy side and lose wakeups. Code built on these barriers
// (BoundedBlockingQueue) is placed in an inline namespace named after the
// variant, which turns some mixes (a queue passed by reference to a
// function of the other variant) into link errors, but not all: a queue
// held as a member of a type shared across units still links.
#if IBMW_SELFDEV_HAS_MEMBARRIER
  #define IBMW_SELFDEV_BARRIER_NS membarrier_v1
#else
  #define IBMW_SELFDEV_BARRIER_NS fence_v1
#endif

namespace ibmw::selfdev::concurrent::detail {
inline namespace IBMW_SELFDEV_BARRIER_NS {

// A store-load fence split between a frequent side (LightBarrier) and a rare
// one (HeavyBarrier): of a light side that stored X and then reads Y, and a
// heavy side that stored Y and then reads X, at least one sees the other's
// store, as with a seq_cst fence on both sides. Where membarrier(2) works,
// the light side is only a compiler barrier and the heavy side a system call
// that runs a full fence on every running thread of the process; elsewhere
// both are seq_cst fences. Define IBMW_SELFDEV_NO_MEMBARRIER to always use
// fences, or set the environment variable IBMW_SELFDEV_NO_MEMBARRIER (to
// anything but "" or "0") before the first queue is made: then membarrier(2)
// is never called, for sandboxes (seccomp) that kill a process using it.

// True once this process is registered for MEMBARRIER_CMD_PRIVATE_EXPEDITED
// (registration is per process and survives fork). Read on every push and
// pop: on its own cache line, so that a neighbouring hot variable of the
// program (seen: a task counter) does not turn each read into a miss.
alignas(64) inline std::atomic<bool> membarrier_ready{false};

// Registers once; returns whether the heavy side may use membarrier(2). Call
// before relying on LightBarrier() being cheap.
inline bool RegisterMembarrier() noexcept {
#if IBMW_SELFDEV_HAS_MEMBARRIER
  static const bool ok = [] {
    const char* off = std::getenv("IBMW_SELFDEV_NO_MEMBARRIER");
    if (off != nullptr && off[0] != '\0' && std::strcmp(off, "0") != 0) return false;
    const long cmds = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    const bool r = cmds >= 0 && (cmds & MEMBARRIER_CMD_PRIVATE_EXPEDITED) != 0 &&
                   syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
    membarrier_ready.store(r, std::memory_order_seq_cst);
    return r;
  }();
  return ok;
#else
  return false;
#endif
}

// A light side that read membarrier_ready as false runs a real fence, so it
// pairs with any heavy side.
inline void LightBarrier() noexcept {
  if (membarrier_ready.load(std::memory_order_relaxed)) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
  } else {
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
}

// Tries per heavy barrier. membarrier(2) can fail for a moment: ENOMEM when
// the kernel cannot allocate its CPU mask (CONFIG_CPUMASK_OFFSTACK).
inline constexpr int kMembarrierTries = 4;

// True once membarrier(2) failed after registration with an error that will
// not go away (EPERM from a seccomp filter installed later, ENOSYS, ...):
// later heavy barriers then fail without trying. Transient errors (EINTR,
// EAGAIN, ENOMEM) do not set it, not even when every try failed.
inline std::atomic<bool> membarrier_blocked{false};

#if IBMW_SELFDEV_HAS_MEMBARRIER
// HeavyBarrier() after its first membarrier(2) failed with errno: retries
// transient errors, and reports a failure once. Cold and out of line, so
// that it stays off the push and pop paths.
[[gnu::cold, gnu::noinline]] inline bool RetryMembarrier() noexcept {
  int err = errno;
  for (int n = 1; n < kMembarrierTries && (err == EINTR || err == EAGAIN || err == ENOMEM); ++n) {
    if (syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0) return true;
    err = errno;
  }
  if (err != EINTR && err != EAGAIN && err != ENOMEM) membarrier_blocked.store(true, std::memory_order_relaxed);
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true, std::memory_order_relaxed)) {
    std::fprintf(stderr,
                 "ibmw_selfdev: membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED) failed after registration: %s; "
                 "threads waiting at a queue lane now re-check every millisecond; "
                 "set IBMW_SELFDEV_NO_MEMBARRIER=1 in the environment to use fences instead\n",
                 std::strerror(err));
  }
  return false;
}
#endif

// Returns whether the barrier was made; false only if membarrier(2) fails
// after registration (see membarrier_blocked; or memory pressure on every
// try). Light sides already skip their fence then, so nothing pairs with
// them: the caller must not rely on being signalled, and should re-check
// what it waits for periodically instead (every store becomes visible in
// finite time). Not mprotect(2) on a resident page as a substitute (folly,
// .NET): that runs a barrier on the other CPUs only where the kernel shoots
// their TLB entries down by IPI, and AArch64 (the OHOS target) invalidates
// remote TLBs by broadcast instructions instead. Out of line: it costs a
// system call anyway, and its callers sit on the push and pop paths.
[[nodiscard, gnu::noinline]] inline bool HeavyBarrier() noexcept {
#if IBMW_SELFDEV_HAS_MEMBARRIER
  if (RegisterMembarrier()) {
    if (membarrier_blocked.load(std::memory_order_relaxed)) return false;
    return syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0 || RetryMembarrier();
  }
#endif
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return true;
}

}  // namespace IBMW_SELFDEV_BARRIER_NS
}  // namespace ibmw::selfdev::concurrent::detail
