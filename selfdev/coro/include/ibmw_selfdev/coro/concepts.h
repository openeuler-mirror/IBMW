// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

namespace ibmw::selfdev::coro {

/// Placeholder value for awaitables that produce void (see SyncWait).
struct Unit {
  friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
};

namespace detail {

template <class A>
concept HasMemberCoAwait = requires(A&& a) { static_cast<A&&>(a).operator co_await(); };

template <class A>
concept HasFreeCoAwait = requires(A&& a) { operator co_await(static_cast<A&&>(a)); };

template <class W>
concept Awaiter = requires(W& w, std::coroutine_handle<> h) {
  { w.await_ready() } -> std::convertible_to<bool>;
  w.await_suspend(h);
  w.await_resume();
};

/// Mirrors the awaiter lookup that `co_await a` performs (without
/// await_transform, which selfdev promises do not define).
template <class A>
decltype(auto) GetAwaiter(A&& a) {
  if constexpr (HasMemberCoAwait<A>) {
    return static_cast<A&&>(a).operator co_await();
  } else if constexpr (HasFreeCoAwait<A>) {
    return operator co_await(static_cast<A&&>(a));
  } else {
    return static_cast<A&&>(a);
  }
}

template <class A>
using AwaiterOf = decltype(GetAwaiter(std::declval<A>()));

}  // namespace detail

/// Something that can be `co_await`ed (as an rvalue of type A).
template <class A>
concept Awaitable = detail::Awaiter<std::remove_reference_t<detail::AwaiterOf<A>>>;

/// Type produced by `co_await std::declval<A>()`.
template <Awaitable A>
using AwaitResult = decltype(std::declval<std::remove_reference_t<detail::AwaiterOf<A>>&>().await_resume());

/// AwaitResult with references and cv-qualifiers removed; void stays void.
template <Awaitable A>
using AwaitValue = std::conditional_t<std::is_void_v<AwaitResult<A>>, void,
                                      std::remove_cvref_t<AwaitResult<A>>>;

template <class S>
concept Scheduler = std::copy_constructible<std::remove_cvref_t<S>> && requires(const std::remove_cvref_t<S>& s) {
  { s.schedule() } -> Awaitable;
};

template <class S>
concept TimedScheduler = Scheduler<S> && requires(const std::remove_cvref_t<S>& s, std::chrono::nanoseconds d,
                                                  std::chrono::system_clock::time_point tp) {
  { s.schedule_after(d) } -> Awaitable;
  { s.schedule_at(tp) } -> Awaitable;
};

}  // namespace ibmw::selfdev::coro
