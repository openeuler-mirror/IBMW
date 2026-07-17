// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <exec/inline_scheduler.hpp>

namespace ibmw::co {

using InlineExecutor = exec::inline_scheduler;

}  // namespace ibmw::co

#else

  #include <unifex/inline_scheduler.hpp>

namespace ibmw::co {

using InlineExecutor = unifex::inline_scheduler;

}  // namespace ibmw::co

#endif
