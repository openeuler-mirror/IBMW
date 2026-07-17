// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

#include "cpp/co/task.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/op_base.h"
#include "cpp/context/res/service.h"
#include "cpp/executor/executor.h"
#include "cpp/service/rpc_status.h"

namespace ibmw::context {

class OpSrv : public OpBase {
 public:
  using OpBase::OpBase;

  OpSrv(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept : OpBase(ctx, loc) {}

  template <class Q, class P>
  [[nodiscard]] res::Service<Q, P> Init(std::string_view func_full_name);

  template <class Q, class P, concepts::SupportedServer<Q, P> TServer>
  void ServeInline(const res::Service<Q, P>& srv, TServer server);

  template <class Q, class P, concepts::SupportedServer<Q, P> TServer>
  void ServeOn(const res::Service<Q, P>& srv, ibmw::executor::ExecutorHandle exe, TServer server);

  template <class Q, class P>
  ibmw::co::Task<ibmw::rpc::Status> Serving(const res::Service<Q, P>& srv, ibmw::rpc::ContextRef ctx, const Q& q, P& p);

 private:
  template <class Q, class P, concepts::SupportedServer<Q, P> Func>
  constexpr auto StandardizeServer(Func&& cb);
};

}  // namespace ibmw::context
