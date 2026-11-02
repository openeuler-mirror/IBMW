// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CO-17 host library: defines the Task that the module awaits.

#include "ibmw_selfdev/coro/task.h"

#define DSO_EXPORT __attribute__((visibility("default")))

namespace coro_dso {

// Completes synchronously; its final awaiter runs in this DSO's code.
DSO_EXPORT ibmw::selfdev::coro::Task<int> HostChild() { co_return 1; }

}  // namespace coro_dso

extern "C" DSO_EXPORT void* CoroDsoHostSlot() { return &ibmw::selfdev::coro::detail::CurrentStartRecord(); }
