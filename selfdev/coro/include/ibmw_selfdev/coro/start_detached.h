// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <type_traits>

#include "ibmw_selfdev/coro/async_scope.h"
#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/export.h"

namespace ibmw::selfdev::coro {

namespace detail {

// Process-wide scope (one per process, defined in libibmw_selfdev_coro);
// its destructor (static destruction of that library) closes it and waits
// for all detached work, like the unifex branch's AsyncScopeDeleter.
IBMW_SELFDEV_CORO_API AsyncScope& DetachedScope() noexcept;

struct StartDetachedFn {
  template <class A>
    requires Awaitable<std::decay_t<A>>
  bool operator()(A&& a) const {
    if (DetachedScope().spawn(static_cast<A&&>(a))) return true;
    // The scope is closed: static destruction has started draining it.
    // Same as unifex::async_scope::spawn on a stopped scope: the work is
    // destroyed here without running. Reported through the return value.
    std::decay_t<A> dropped(static_cast<A&&>(a));
    (void)dropped;
    return false;
  }
};

}  // namespace detail

/// Runs `a` on the process-wide scope without waiting for it. Returns true
/// once started. Returns false when the scope is already closed, i.e. while
/// the scope drains during static destruction (e.g. detached work starting
/// more detached work at exit): `a` is then destroyed without running, as
/// with unifex.
///
/// Shutdown ordering: the scope is created by the first StartDetached call
/// in the process and drained by an exit-time destructor of
/// libibmw_selfdev_coro (std::exit or return from main), i.e. in reverse
/// order of construction: statics constructed after that first call are destroyed
/// before the drain, statics constructed before it after the drain.
/// Everything detached work touches must therefore have been constructed
/// before the first StartDetached call (or be awaited explicitly, e.g. with
/// an own AsyncScope, before exit). Calling StartDetached after the drain has
/// finished is undefined behaviour. quick_exit / _exit skip the drain.
///
/// The drain happens at process exit only, never at dlclose() of a package
/// that called StartDetached: IBMW hosts load libibmw_selfdev_coro at
/// start-up, so unloading a package does not unload it. A package must
/// finish (or await through its own AsyncScope) the detached work that runs
/// its code before it is unloaded; work still pending afterwards would run
/// unmapped code.
inline constexpr detail::StartDetachedFn StartDetached{};

}  // namespace ibmw::selfdev::coro
