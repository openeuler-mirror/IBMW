// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CO-17 module: loaded with dlopen (RTLD_LOCAL | RTLD_DEEPBIND), or linked
// with -Bsymbolic / a version script. Awaits a Task defined in another DSO
// in a long synchronous loop: with a per-DSO slot every completion would
// nest one more resume() and the loop would overflow the stack.

#include "ibmw_selfdev/coro/sync_wait.h"
#include "ibmw_selfdev/coro/task.h"

#define DSO_EXPORT __attribute__((visibility("default")))

namespace coro_dso {

DSO_EXPORT ibmw::selfdev::coro::Task<int> HostChild();

namespace {

ibmw::selfdev::coro::Task<long> Loop(int n) {
  long sum = 0;
  for (int i = 0; i < n; ++i) sum += co_await HostChild();
  co_return sum;
}

}  // namespace

}  // namespace coro_dso

extern "C" DSO_EXPORT void* CoroDsoModuleSlot() { return &ibmw::selfdev::coro::detail::CurrentStartRecord(); }

extern "C" DSO_EXPORT long CoroDsoModuleLoop(int n) {
  return *ibmw::selfdev::coro::SyncWait(coro_dso::Loop(n));
}
