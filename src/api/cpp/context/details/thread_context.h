// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cpp/executor/executor.h>
#include <cpp/service/rpc_context.h>
#include <base/exception.h>
#include <base/source_location.h>

#include <memory>
#include <optional>

namespace ibmw::context {
class Context;
}  // namespace ibmw::context

namespace ibmw::context::details {

struct ThreadContext {
  std::weak_ptr<Context> ctx_ptr;
  ibmw::executor::ExecutorHandle exe;
  std::optional<ibmw::rpc::ContextRef> active_rpc_context;
};

inline thread_local ThreadContext g_thread_ctx;

// Restores g_thread_ctx to its value at construction on scope exit.
class ScopedThreadContextRestore {
 public:
  ScopedThreadContextRestore() : prev_(g_thread_ctx) {}
  ~ScopedThreadContextRestore() { g_thread_ctx = std::move(prev_); }
  ScopedThreadContextRestore(const ScopedThreadContextRestore&) = delete;
  ScopedThreadContextRestore& operator=(const ScopedThreadContextRestore&) = delete;

 private:
  ThreadContext prev_;
};

inline std::shared_ptr<Context> GetCurrentContext(const ibmw::common::util::SourceLocation& call_loc = ibmw::common::util::SourceLocation::current()) {
  const std::shared_ptr ctx_ptr = g_thread_ctx.ctx_ptr.lock();

  if (ctx_ptr == nullptr) [[unlikely]]
    MW_ASSERT_LOC(call_loc, ctx_ptr != nullptr, "Broken context !");
  return ctx_ptr;
}

}  // namespace ibmw::context::details
