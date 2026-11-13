// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <exec/task.hpp>

namespace ibmw::co {

template <typename T>
using Task = typename exec::task<T>;

}

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/task.h"

namespace ibmw::co {

template <typename T>
using Task = typename ibmw::selfdev::coro::Task<T>;

}

#else

  #include <unifex/task.hpp>

namespace ibmw::co {

template <typename T>
using Task = typename unifex::task<T>;

}

#endif
