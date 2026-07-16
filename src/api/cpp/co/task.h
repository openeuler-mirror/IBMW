// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <exec/task.hpp>

namespace ibmw::co {

template <typename T>
using Task = typename exec::task<T>;

}

#else

  #include <unifex/task.hpp>

namespace ibmw::co {

template <typename T>
using Task = typename unifex::task<T>;

}

#endif
