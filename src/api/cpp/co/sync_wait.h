// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& SyncWait = stdexec::sync_wait;

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/sync_wait.h"

namespace ibmw::co {

inline constexpr auto& SyncWait = ibmw::selfdev::coro::SyncWait;

}  // namespace ibmw::co

#else

  #include <unifex/sync_wait.hpp>

namespace ibmw::co {

inline constexpr auto& SyncWait = unifex::sync_wait;

}  // namespace ibmw::co

#endif
