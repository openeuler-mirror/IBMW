// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <exec/inline_scheduler.hpp>

namespace ibmw::co {

using InlineExecutor = exec::inline_scheduler;

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/scheduler.h"

namespace ibmw::co {

using InlineExecutor = ibmw::selfdev::coro::InlineScheduler;

}  // namespace ibmw::co

#else

  #include <unifex/inline_scheduler.hpp>

namespace ibmw::co {

using InlineExecutor = unifex::inline_scheduler;

}  // namespace ibmw::co

#endif
