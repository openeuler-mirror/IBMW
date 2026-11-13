// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/coro/start_detached.h"

#include "ibmw_selfdev/coro/sync_wait.h"

namespace ibmw::selfdev::coro::detail {

// Defined here, not inline in the header: one process-wide scope for every
// DSO (an inline function-local static is duplicated per DSO under
// RTLD_DEEPBIND / -Bsymbolic, and is an STB_GNU_UNIQUE symbol with GCC).
AsyncScope& DetachedScope() noexcept {
  struct Holder {
    AsyncScope scope;
    // Exit-time destruction: closes the scope (later StartDetached calls return
    // false and drop their work) and waits for all detached work.
    ~Holder() { SyncWait(scope.cleanup()); }
  };
  static Holder holder;
  return holder.scope;
}

}  // namespace ibmw::selfdev::coro::detail
