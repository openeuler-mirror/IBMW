// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_USE_FMT_LIB
  #include "fmt/chrono.h"
  #include "fmt/core.h"
  #include "fmt/format.h"

  #define ibmw_fmt fmt

#else
  #include <format>

  #define ibmw_fmt std
#endif
