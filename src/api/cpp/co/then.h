// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& Then = stdexec::then;

}  // namespace ibmw::co

#else

  #include <unifex/then.hpp>

namespace ibmw::co {

inline constexpr auto& Then = unifex::then;

}  // namespace ibmw::co

#endif
