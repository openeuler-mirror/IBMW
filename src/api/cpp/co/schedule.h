// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <exec/timed_scheduler.hpp>
  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& Schedule = stdexec::schedule;
inline constexpr auto& ScheduleAfter = exec::schedule_after;
inline constexpr auto& ScheduleAt = exec::schedule_at;

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/scheduler.h"

namespace ibmw::co {

inline constexpr auto& Schedule = ibmw::selfdev::coro::Schedule;
inline constexpr auto& ScheduleAfter = ibmw::selfdev::coro::ScheduleAfter;
inline constexpr auto& ScheduleAt = ibmw::selfdev::coro::ScheduleAt;

}  // namespace ibmw::co

#else

  #include <unifex/scheduler_concepts.hpp>

namespace ibmw::co {

inline constexpr auto& Schedule = unifex::schedule;
inline constexpr auto& ScheduleAfter = unifex::schedule_after;
inline constexpr auto& ScheduleAt = unifex::schedule_at;

}  // namespace ibmw::co

#endif
