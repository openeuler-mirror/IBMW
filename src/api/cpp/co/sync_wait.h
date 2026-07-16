// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& SyncWait = stdexec::sync_wait;

}  // namespace ibmw::co

#else

  #include <unifex/sync_wait.hpp>

namespace ibmw::co {

inline constexpr auto& SyncWait = unifex::sync_wait;

}  // namespace ibmw::co

#endif
