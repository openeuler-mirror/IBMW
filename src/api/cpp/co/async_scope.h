// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <exec/async_scope.hpp>

namespace ibmw::co {

using AsyncScope = exec::async_scope;

  #define complete on_empty

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/async_scope.h"

namespace ibmw::co {

using AsyncScope = ibmw::selfdev::coro::AsyncScope;

}  // namespace ibmw::co

#else

  #include <unifex/async_scope.hpp>

namespace ibmw::co {

using AsyncScope = unifex::async_scope;

}  // namespace ibmw::co

#endif
