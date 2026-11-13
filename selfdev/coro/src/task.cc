// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/task.h"

namespace ibmw::selfdev::coro::detail {

namespace {

// The only definition in the process (this file is built into the shared
// library libibmw_selfdev_coro; never compile it into more than one DSO).
#if defined(IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC)
[[gnu::tls_model("initial-exec")]]
#endif
thread_local StartRecord* g_current_start = nullptr;

// Start() runs on every co_await of a Task. Its only address-taken local is
// the two-word StartRecord (no arrays, nothing written through an index), so
// the stack-protector canary that -fstack-protector-strong adds for it guards
// nothing and costs a load, a store and a compare per co_await.
#if defined(__has_attribute)
  #if __has_attribute(no_stack_protector)
    #define IBMW_SELFDEV_CORO_NO_SSP __attribute__((no_stack_protector))
  #endif
#endif
#if !defined(IBMW_SELFDEV_CORO_NO_SSP)
  #define IBMW_SELFDEV_CORO_NO_SSP
#endif

}  // namespace

StartRecord*& CurrentStartRecord() noexcept { return g_current_start; }

IBMW_SELFDEV_CORO_NO_SSP bool TaskPromiseBase::Start(std::coroutine_handle<> self, std::coroutine_handle<> awaiting) noexcept {
  continuation_ = awaiting;
  StartRecord record{this, false};
  start_ = &record;
  StartRecord* const outer = g_current_start;
  g_current_start = &record;
  self.resume();
  // From here on `self` may be destroyed (it may have completed on another
  // thread): only locals and this thread's slot. The slot is re-read here,
  // after the call, never cached across it.
  g_current_start = outer;
  return !record.done_inline;
}

std::coroutine_handle<> TaskPromiseBase::Finish() noexcept {
  StartRecord* const start = g_current_start;
  // Both directions must match: a new task allocated at the address of a
  // finished one, started on another thread, then completing here while the
  // old Start() is still on this stack, has the same promise address but
  // points at a different record.
  if (start != nullptr && start->promise == this && start_ == start) {
    start->done_inline = true;
    return std::noop_coroutine();
  }
  return continuation_;
}

}  // namespace ibmw::selfdev::coro::detail
