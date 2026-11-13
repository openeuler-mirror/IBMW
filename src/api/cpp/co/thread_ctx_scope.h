// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <utility>

#include "cpp/context/details/thread_context.h"

namespace ibmw::co::details {

// Thread context for resuming a coroutine on an executor's thread: sets
// g_thread_ctx.exe to that executor and restores the thread's whole previous
// context on destruction, also when resume() throws (a coroutine type whose
// unhandled_exception rethrows). Only exe is set: ctx_ptr and
// active_rpc_context stay what the worker thread has, so code after co_await
// that needs the module context (GetCurrentContext()) calls
// Context::LetMe() first. Used by the coroutine resumers of
// co::IbmwExecutor (ibmw_context.h) and of CallAwaitable (rpc_handle.h).
class ThreadCtxScope {
 public:
  explicit ThreadCtxScope(const ibmw::executor::ExecutorHandle& exe)
      : previous_(ibmw::context::details::g_thread_ctx) {
    ibmw::context::details::g_thread_ctx.exe = exe;
  }
  ~ThreadCtxScope() { ibmw::context::details::g_thread_ctx = std::move(previous_); }

  ThreadCtxScope(const ThreadCtxScope&) = delete;
  ThreadCtxScope& operator=(const ThreadCtxScope&) = delete;

 private:
  ibmw::context::details::ThreadContext previous_;
};

}  // namespace ibmw::co::details
