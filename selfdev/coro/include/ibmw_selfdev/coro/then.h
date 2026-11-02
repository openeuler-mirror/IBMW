// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <type_traits>
#include <utility>

#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/on.h"
#include "ibmw_selfdev/coro/scheduler.h"
#include "ibmw_selfdev/coro/task.h"

namespace ibmw::selfdev::coro {

namespace detail {

// f receives exactly what `co_await a` yields (AwaitResult, not decayed), so
// f(int&) works with an awaitable yielding int& and f(int&&) with Task<int>.
template <class A, class F>
struct ThenResult {
  using type = std::invoke_result_t<F&, AwaitResult<A>>;
};

template <class A, class F>
  requires std::is_void_v<AwaitValue<A>>
struct ThenResult<A, F> {
  using type = std::invoke_result_t<F&>;
};

template <class A, class F>
using ThenResultT = std::remove_cvref_t<typename ThenResult<A, F>::type>;

// Scheduler argument of ThenImpl when there is nothing to switch to.
struct NoSwitch {};

// Then(a, f), and On(s, a) | Then(f) in the same frame (one allocation
// instead of two: this is the shape of IBMW's generated RPC handlers).
template <class S, class A, class F>
Task<ThenResultT<A, F>> ThenImpl(S s, A a, F f) {
  using R = ThenResultT<A, F>;
  if constexpr (!std::is_same_v<S, NoSwitch>) co_await Schedule(s);
  if constexpr (std::is_void_v<AwaitValue<A>>) {
    co_await std::move(a);
    if constexpr (std::is_void_v<R>) {
      std::invoke(f);
    } else {
      co_return std::invoke(f);
    }
  } else {
    if constexpr (std::is_void_v<R>) {
      std::invoke(f, co_await std::move(a));
    } else {
      co_return std::invoke(f, co_await std::move(a));
    }
  }
}

/// Argument a of Then(a, f) / a | Then(f): an awaitable that is passed as an
/// rvalue or can be copied. An lvalue Task (move-only) is rejected here
/// instead of failing deep inside ThenImpl; write std::move(task).
template <class A>
concept ThenSource = Awaitable<std::decay_t<A>> &&
                     (!std::is_lvalue_reference_v<A> || std::copy_constructible<std::decay_t<A>>);

template <class T>
struct IsOnAwaitable : std::false_type {};

template <class S, class A>
struct IsOnAwaitable<OnAwaitable<S, A>> : std::true_type {
  using scheduler = S;
  using inner = A;
};

template <class A, class F>
auto MakeThen(A&& a, F&& f) {
  static_assert(ThenSource<A>, "Then: pass a move-only awaitable (e.g. Task) as an rvalue: std::move(task)");
  using DA = std::decay_t<A>;
  using DF = std::decay_t<F>;
  if constexpr (IsOnAwaitable<DA>::value && !std::is_lvalue_reference_v<A>) {
    using S = typename IsOnAwaitable<DA>::scheduler;
    using In = typename IsOnAwaitable<DA>::inner;
    return ThenImpl<S, In, DF>(static_cast<A&&>(a).TakeScheduler(), static_cast<A&&>(a).TakeInner(),
                               static_cast<F&&>(f));
  } else {
    return ThenImpl<NoSwitch, DA, DF>(NoSwitch{}, static_cast<A&&>(a), static_cast<F&&>(f));
  }
}

template <class F>
class ThenClosure {
 public:
  explicit ThenClosure(F f) : f_(std::move(f)) {}

  /// `a | Then(f)`; found by ADL through ThenClosure.
  template <class A>
    requires ThenSource<A>
  friend auto operator|(A&& a, ThenClosure closure) {
    return MakeThen(static_cast<A&&>(a), std::move(closure.f_));
  }

 private:
  F f_;
};

struct ThenFn {
  template <class A, class F>
    requires ThenSource<A>
  auto operator()(A&& a, F&& f) const {
    return MakeThen(static_cast<A&&>(a), static_cast<F&&>(f));
  }

  template <class F>
  ThenClosure<std::decay_t<F>> operator()(F&& f) const {
    return ThenClosure<std::decay_t<F>>(static_cast<F&&>(f));
  }
};

}  // namespace detail

/// Then(a, f): awaits a, then returns f(result) (or f() for void).
/// Then(f): closure for the pipe form `a | Then(f)`.
inline constexpr detail::ThenFn Then{};

}  // namespace ibmw::selfdev::coro
