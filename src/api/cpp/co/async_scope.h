// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <exec/async_scope.hpp>

namespace ibmw::co {

using AsyncScope = exec::async_scope;

  #define complete on_empty

}  // namespace ibmw::co

#else

  #include <unifex/async_scope.hpp>

namespace ibmw::co {

using AsyncScope = unifex::async_scope;

}  // namespace ibmw::co

#endif
