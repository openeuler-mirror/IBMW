// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& On = stdexec::on;

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/on.h"

namespace ibmw::co {

inline constexpr auto& On = ibmw::selfdev::coro::On;

}  // namespace ibmw::co

#else

  #include <unifex/on.hpp>

namespace ibmw::co {

inline constexpr auto& On = unifex::on;

}  // namespace ibmw::co

#endif
