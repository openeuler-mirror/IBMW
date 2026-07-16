// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <optional>

#include "base/source_location.h"
#include "cpp/co/task.h"
#include "cpp/context/res/base.h"
#include "cpp/executor/executor.h"
#include "cpp/service/rpc_context.h"
#include "cpp/service/rpc_status.h"

namespace ibmw::context::res {

template <class Q, class P>
class Service : public details::Base {
 public:
  using RequestType = Q;
  using ResponseType = P;
  using details::Base::Base;
};

template <class Q, class P>
class Client : public res::Service<Q, P> {
 public:
  using ClientRequest = Q;
  using ClientResponse = P;

  [[nodiscard]] co::Task<ibmw::rpc::Status> Call(
      const Q& q, P& p, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;

  [[nodiscard]] co::Task<ibmw::rpc::Status> operator()(
      const Q& q, P& p, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    return Call(q, p, loc);
  }

  [[nodiscard]] co::Task<std::optional<P>> Call(
      const Q& q, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    P p;
    if (const ibmw::rpc::Status status = co_await Call(q, p, loc); status.OK())
      co_return p;

    co_return {};
  }

  [[nodiscard]] co::Task<std::optional<P>> operator()(
      const Q& q, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    return Call(q, loc);
  }

  [[nodiscard]] co::Task<ibmw::rpc::Status> Call(
      ibmw::rpc::ContextRef ctx, const Q& q, P& p,
      ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;

  [[nodiscard]] co::Task<ibmw::rpc::Status> operator()(
      ibmw::rpc::ContextRef ctx, const Q& q, P& p,
      ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    return Call(std::move(ctx), q, p, loc);
  }

  [[nodiscard]] co::Task<std::optional<P>> Call(
      ibmw::rpc::ContextRef ctx, const Q& q,
      ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    P p;
    if (const ibmw::rpc::Status status = co_await Call(std::move(ctx), q, p, loc); status.OK())
      co_return p;

    co_return {};
  }

  [[nodiscard]] co::Task<std::optional<P>> operator()(
      ibmw::rpc::ContextRef ctx, const Q& q,
      ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    return Call(std::move(ctx), q, loc);
  }

 public:
  Client() = default;
  using res::Service<Q, P>::Service;

  Client(res::Service<Q, P>&& res)
      : res::Service<Q, P>(std::move(res)) {}

  Client(const res::Service<Q, P>& res)
      : res::Service<Q, P>(res) {}
};

template <class Q, class P>
class Server : public res::Service<Q, P> {
 public:
  using ServerRequest = Q;
  using ServerResponse = P;

  template <typename TServer>
  void ServeInline(TServer server, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;

  template <typename TServer>
  void ServeOn(const ibmw::executor::ExecutorHandle& exe, TServer server,
               ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;

 public:
  Server() = default;
  using res::Service<Q, P>::Service;

  Server(res::Service<Q, P>&& res)
      : res::Service<Q, P>(std::move(res)) {}

  Server(const res::Service<Q, P>& res)
      : res::Service<Q, P>(res) {}
};

}  // namespace ibmw::context::res
