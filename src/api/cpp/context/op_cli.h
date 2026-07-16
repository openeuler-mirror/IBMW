// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "cpp/co/task.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/op_base.h"
#include "cpp/context/res/service.h"
#include "cpp/service/rpc_context.h"
#include "cpp/service/rpc_status.h"

namespace ibmw::context {

class OpCli : public OpBase {
 public:
  using OpBase::OpBase;

  OpCli(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept : OpBase(ctx, loc) {}

  template <class Q, class P, concepts::RawClient<Q, P> TClient>
  [[nodiscard]] res::Client<Q, P> Init(std::string_view service_name, TClient&& client);

  template <class Q, class P>
  co::Task<ibmw::rpc::Status> Call(const res::Service<Q, P>& srv, const Q& q, P& p);

  template <class Q, class P>
  co::Task<ibmw::rpc::Status> Call(const res::Service<Q, P>& srv, ibmw::rpc::ContextRef ctx, const Q& q, P& p);

 private:
  template <class Q, class P>
  std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)> CreateCallFunction(std::string_view func_full_name) const;
};

}  // namespace ibmw::context
