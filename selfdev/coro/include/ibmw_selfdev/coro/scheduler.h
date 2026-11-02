// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <coroutine>

namespace ibmw::selfdev::coro {

namespace detail {

struct ScheduleFn {
  template <class S>
    requires requires(S&& s) { static_cast<S&&>(s).schedule(); }
  auto operator()(S&& s) const noexcept(noexcept(static_cast<S&&>(s).schedule())) {
    return static_cast<S&&>(s).schedule();
  }
};

struct ScheduleAfterFn {
  template <class S, class D>
    requires requires(S&& s, D&& d) { static_cast<S&&>(s).schedule_after(static_cast<D&&>(d)); }
  auto operator()(S&& s, D&& d) const
      noexcept(noexcept(static_cast<S&&>(s).schedule_after(static_cast<D&&>(d)))) {
    return static_cast<S&&>(s).schedule_after(static_cast<D&&>(d));
  }
};

struct ScheduleAtFn {
  template <class S, class Tp>
    requires requires(S&& s, Tp&& tp) { static_cast<S&&>(s).schedule_at(static_cast<Tp&&>(tp)); }
  auto operator()(S&& s, Tp&& tp) const
      noexcept(noexcept(static_cast<S&&>(s).schedule_at(static_cast<Tp&&>(tp)))) {
    return static_cast<S&&>(s).schedule_at(static_cast<Tp&&>(tp));
  }
};

}  // namespace detail

/// `co_await Schedule(s)` continues on s.
inline constexpr detail::ScheduleFn Schedule{};
/// `co_await ScheduleAfter(s, d)` continues on s after d.
inline constexpr detail::ScheduleAfterFn ScheduleAfter{};
/// `co_await ScheduleAt(s, tp)` continues on s at tp.
inline constexpr detail::ScheduleAtFn ScheduleAt{};

/// Completes immediately on the current thread.
class InlineScheduler {
 public:
  std::suspend_never schedule() const noexcept { return {}; }

  friend constexpr bool operator==(InlineScheduler, InlineScheduler) noexcept { return true; }
};

}  // namespace ibmw::selfdev::coro
