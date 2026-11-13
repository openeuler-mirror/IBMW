// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <type_traits>
#include <utility>

#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/scheduler.h"
#include "ibmw_selfdev/coro/task.h"

namespace ibmw::selfdev::coro {

namespace detail {

template <class S, class A>
Task<AwaitValue<A>> OnImpl(S s, A a) {
  co_await Schedule(s);
  if constexpr (std::is_void_v<AwaitValue<A>>) {
    co_await std::move(a);
  } else {
    co_return co_await std::move(a);
  }
}

/// Result of On(s, a). Holds s and a until it is awaited, so that
/// `On(s, a) | Then(f)` can run as one coroutine frame instead of two (see
/// then.h). Awaiting it, or converting it to a Task, creates the OnImpl frame.
template <class S, class A>
class [[nodiscard]] OnAwaitable {
 public:
  using value_type = AwaitValue<A>;

  template <class S2, class A2>
  OnAwaitable(S2&& s, A2&& a) : s_(static_cast<S2&&>(s)), a_(static_cast<A2&&>(a)) {}

  auto operator co_await() && { return OnImpl<S, A>(std::move(s_), std::move(a_)).operator co_await(); }

  // NOLINTNEXTLINE(google-explicit-constructor): On() used to return a Task.
  operator Task<value_type>() && { return OnImpl<S, A>(std::move(s_), std::move(a_)); }

  S&& TakeScheduler() && noexcept { return std::move(s_); }
  A&& TakeInner() && noexcept { return std::move(a_); }

  // A const rvalue (std::move of a const OnAwaitable) cannot give up s and a.
  void operator co_await() const&& = delete;
  operator Task<value_type>() const&& = delete;
  void TakeScheduler() const&& = delete;
  void TakeInner() const&& = delete;

 private:
  S s_;
  A a_;
};

struct OnFn {
  template <Scheduler S, class A>
    requires Awaitable<std::decay_t<A>>
  auto operator()(S&& s, A&& a) const {
    return OnAwaitable<std::decay_t<S>, std::decay_t<A>>(static_cast<S&&>(s), static_cast<A&&>(a));
  }
};

}  // namespace detail

/// Switches to scheduler s, then awaits a there; yields a's result. The
/// continuation runs wherever a completes. Nothing runs (and nothing is
/// allocated) until the result is awaited, spawned or SyncWait'ed.
inline constexpr detail::OnFn On{};

}  // namespace ibmw::selfdev::coro
