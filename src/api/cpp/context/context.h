// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <any>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/source_location.h"
#include "cpp/transport/transport_handle.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/schedule.h"
#include "cpp/co/task.h"
#include "cpp/context/transport_context.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/details/type_support.h"
#include "cpp/context/res/transport.h"
#include "cpp/context/res/service.h"
#include "cpp/context/rpc_context.h"
#include "cpp/core.h"
#include "cpp/executor/executor.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/stable_vector.h"

#include "base/exception.h"

#include "cpp/context/details/thread_context.h"

namespace ibmw::context {

class OpPub;
class OpSub;
class OpCli;
class OpSrv;
class OpLog;

class Context : public std::enable_shared_from_this<Context> {
 public:
  // Does not make itself current: weak_from_this() is still empty inside the
  // constructor, so that would only clear the thread's current context.
  Context() noexcept
      : id_(++global_unique_id_) {}

  explicit Context(ibmw::Runtime core) noexcept
      : core_(core), id_(++global_unique_id_) {}

  ~Context() = default;

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  ibmw::Runtime GetRawRef() const noexcept { return core_; }
  static ibmw::Runtime GetRawRef(const Context& ctx) noexcept { return ctx.core_; }

  [[nodiscard]] ibmw::logger::LoggerRef GetLogger() const { return core_.GetLogger(); }
  static ibmw::logger::LoggerRef GetLogger(const Context& ctx) { return ctx.core_.GetLogger(); }

  std::string_view GetConfigFilePath() const { return core_.GetConfigurator().GetConfigFilePath(); }

  void LetMe() {
    auto exe = details::g_thread_ctx.exe;
    details::g_thread_ctx = {.ctx_ptr = weak_from_this(), .exe = exe};
  }

  void StopRunning() noexcept { is_running_ = false; }

  void SetPubState(TransportState state) noexcept { pub_state_ = state; }
  TransportState GetPubState() const noexcept { return pub_state_; }

  void SetSubState(TransportState state) noexcept { sub_state_ = state; }
  TransportState GetSubState() const noexcept { return sub_state_; }

  void SetCliState(RpcState state) noexcept { cli_state_ = state; }
  RpcState GetCliState() const noexcept { return cli_state_; }

  void SetSrvState(RpcState state) noexcept { srv_state_ = state; }
  RpcState GetSrvState() const noexcept { return srv_state_; }

  [[nodiscard]] bool Running() const noexcept {
    return is_running_;
  }

  [[nodiscard]] OpPub pub(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());
  [[nodiscard]] OpSub sub(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());
  [[nodiscard]] OpCli cli(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());
  [[nodiscard]] OpSrv srv(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());
  [[nodiscard]] OpLog log(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  [[nodiscard]] ibmw::executor::ExecutorHandle CreateExecutor(std::string_view name, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const {
    MW_ASSERT_LOC(loc, core_, "Core reference is null when get executor [{}].", name);
    auto ex = core_.GetExecutor().GetExecutor(name);
    MW_ASSERT_LOC(loc, ex, "Get executor [{}] failed.", name);
    return ex;
  }

  // CreatePublisher helpers
  template <class T>
  [[nodiscard]] res::Publisher<T> CreatePublisher(std::string_view topic_name, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  // CreateSubscriber helpers
  template <class T>
  [[nodiscard]] res::Subscriber<T> CreateSubscriber(std::string_view topic_name, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  template <class T, concepts::SupportedSubscriber<T> TCallback>
  [[nodiscard]] res::Subscriber<T> CreateSubscriber(std::string_view topic_name, TCallback&& callback, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  template <class T, concepts::SupportedSubscriber<T> TCallback>
  [[nodiscard]] res::Subscriber<T> CreateSubscriber(std::string_view topic_name, const ibmw::executor::ExecutorHandle& exe, TCallback&& callback, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  // CreateClient for service client class
  template <class TServiceClient>
  [[nodiscard]] TServiceClient CreateClient(std::string_view service_name = "", ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  // CreateServer for service server class
  template <class TServiceServer>
  [[nodiscard]] std::shared_ptr<TServiceServer> CreateServer(std::string_view service_name = "", ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

  // Extract client call function from RPC resource
  template <class Q, class P>
  std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)>
  GetClientFunction(const res::Service<Q, P>& srv, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current());

 private:
  friend class OpPub;
  friend class OpSub;
  friend class OpCli;
  friend class OpSrv;

  template <class T>
  TransportResource& GetTransportResource(const res::Transport<T>& res, ibmw::common::util::SourceLocation loc);

  template <class Q, class P>
  RpcResource& GetRpcResource(const res::Service<Q, P>& res, ibmw::common::util::SourceLocation loc);

  template <class T>
  using SendFunction = std::function<void(ibmw::transport::SenderRef, ibmw::transport::ContextRef, const T&)>;

  template <class T>
  using TransportCallback = std::function<void(ibmw::transport::ContextRef, std::shared_ptr<const T>)>;

  template <class T>
  using SubscribeFunction = std::function<bool(
      ibmw::transport::ReceiverRef,
      TransportCallback<T>,
      ibmw::executor::ExecutorHandle)>;

  template <class Q, class P>
  using Client = std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)>;

  template <class Q, class P>
  using Server = std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)>;

  template <class Q, class P>
  using ServerInvoker = std::function<
      ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&, std::any&)>;

 private:
  ibmw::Runtime core_;
  std::atomic_bool is_running_{true};
  std::atomic_bool enable_trace_{false};
  std::atomic<TransportState> pub_state_{TransportState::kOn};
  std::atomic<TransportState> sub_state_{TransportState::kOn};
  std::atomic<RpcState> cli_state_{RpcState::kOn};
  std::atomic<RpcState> srv_state_{RpcState::kOn};
  ibmw::util::DynamicLatch dynamic_latch_;
  int id_{0};

  // Stable storage: resources may be created at runtime while callback threads
  // hold references obtained via GetTransportResource/GetRpcResource.
  ibmw::util::StableVector<TransportResource> transport_resources_;
  ibmw::util::StableVector<RpcResource> rpc_resources_;

  inline static std::atomic_int global_unique_id_{0};
};

template <class T>
auto Context::GetTransportResource(const res::Transport<T>& res, ibmw::common::util::SourceLocation loc) -> TransportResource& {
  MW_ASSERT_LOC(loc, res.IsValid(), "Transport [{}] is invalid.", res.GetName());
  MW_ASSERT_LOC(loc, res.context_id_ == id_, "Transport [{}] belongs to context [{}], but current context is [{}].", res.GetName(), res.context_id_, id_);
  MW_ASSERT_LOC(loc, res.idx_ < transport_resources_.size(), "Transport [{}] index [{}] out of range (size = {}).", res.GetName(), res.idx_, transport_resources_.size());
  return transport_resources_[res.idx_];
}

template <class Q, class P>
auto Context::GetRpcResource(const res::Service<Q, P>& res, ibmw::common::util::SourceLocation loc) -> RpcResource& {
  MW_ASSERT_LOC(loc, res.IsValid(), "Service [{}] is invalid.", res.GetName());
  MW_ASSERT_LOC(loc, res.context_id_ == id_, "Service [{}] belongs to context [{}], but current context is [{}].", res.GetName(), res.context_id_, id_);
  MW_ASSERT_LOC(loc, res.idx_ < rpc_resources_.size(), "Service [{}] index [{}] out of range (size = {}).", res.GetName(), res.idx_, rpc_resources_.size());
  return rpc_resources_[res.idx_];
}

}  // namespace ibmw::context

namespace ibmw::context {

inline bool Running(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) {
  return details::GetCurrentContext(loc)->Running();
}

}  // namespace ibmw::context

#include "cpp/context/op_cli.h"
#include "cpp/context/op_log.h"
#include "cpp/context/op_pub.h"
#include "cpp/context/op_srv.h"
#include "cpp/context/op_sub.h"

namespace ibmw::context {

// Context member function implementations
inline OpPub Context::pub(ibmw::common::util::SourceLocation loc) {
  return OpPub(*this, loc);
}

inline OpSub Context::sub(ibmw::common::util::SourceLocation loc) {
  return OpSub(*this, loc);
}

inline OpCli Context::cli(ibmw::common::util::SourceLocation loc) {
  return OpCli(*this, loc);
}

inline OpSrv Context::srv(ibmw::common::util::SourceLocation loc) {
  return OpSrv(*this, loc);
}

inline OpLog Context::log(ibmw::common::util::SourceLocation loc) {
  return OpLog(*this, loc);
}

// Context CreatePublisher helper impl
template <class T>
inline res::Publisher<T> Context::CreatePublisher(std::string_view topic_name, ibmw::common::util::SourceLocation loc) {
  return pub(loc).Init<T>(topic_name);
}

// publisher init
template <class T>
std::pair<res::Publisher<T>, TransportResource&> OpPub::DoInit(std::string_view topic_name) {
  TransportResource transport_ctx;
  transport_ctx.pub = ctx_.core_.GetTransport().GetPublisher(topic_name);
  MW_ASSERT_LOC(loc_, transport_ctx.pub, "Get publisher for topic [{}] failed.", topic_name);

  const bool registered = transport_ctx.pub.RegisterSendType(ibmw::GetMessageTypeSupport<T>());
  MW_ASSERT_LOC(loc_, registered, "Register publish type for topic [{}] failed.", topic_name);

  const size_t idx = ctx_.transport_resources_.emplace_back(std::move(transport_ctx));

  res::Publisher<T> res;
  res.name_ = std::string(topic_name);
  res.idx_ = idx;
  res.context_id_ = ctx_.id_;
  return {res, ctx_.transport_resources_[idx]};
}

// publisher publish
template <class T>
void OpPub::Send(const res::Transport<T>& ch, ibmw::transport::ContextRef ch_ctx, const T& msg) {
  auto& transport_ctx = ctx_.GetTransportResource(ch, loc_);
  if (!ctx_.Running() || ctx_.GetPubState() != context::TransportState::kOn) [[unlikely]] {
    ctx_.log().Trace("Publisher is not ready.");
    return;
  }
  std::any_cast<typename Context::SendFunction<T>&>(transport_ctx.pub_f)(transport_ctx.pub, ch_ctx, msg);
}

template <class T>
void OpPub::Send(const res::Transport<T>& ch, const T& msg) {
  ibmw::transport::Context ctx;
  Send(ch, ctx, msg);
}

template <class T>
Context::SendFunction<T> OpPub::CreateSendFunction() {
  return [](ibmw::transport::SenderRef publisher, ibmw::transport::ContextRef ctx_ref, const T& msg) {
    ibmw::transport::PublishMsg(publisher, ctx_ref, msg);
  };
}

// subscriber init
template <class T>
std::pair<res::Subscriber<T>, TransportResource&> OpSub::DoInit(std::string_view topic_name) {
  TransportResource transport_ctx;
  transport_ctx.sub = ctx_.core_.GetTransport().GetSubscriber(topic_name);
  MW_ASSERT_LOC(loc_, transport_ctx.sub, "Get subscriber for topic [{}] failed.", topic_name);

  const size_t idx = ctx_.transport_resources_.emplace_back(std::move(transport_ctx));

  res::Subscriber<T> res;
  res.name_ = std::string(topic_name);
  res.idx_ = idx;
  res.context_id_ = ctx_.id_;
  return {res, ctx_.transport_resources_[idx]};
}

// Context CreateSubscriber helpers impls
template <class T>
inline res::Subscriber<T> Context::CreateSubscriber(std::string_view topic_name, ibmw::common::util::SourceLocation loc) {
  return sub(loc).Init<T>(topic_name);
}

template <class T, concepts::SupportedSubscriber<T> TCallback>
inline res::Subscriber<T> Context::CreateSubscriber(std::string_view topic_name, TCallback&& callback, ibmw::common::util::SourceLocation loc) {
  auto s = sub(loc).Init<T>(topic_name);
  sub(loc).SubscribeInline(s, std::forward<TCallback>(callback));
  return s;
}

template <class T, concepts::SupportedSubscriber<T> TCallback>
inline res::Subscriber<T> Context::CreateSubscriber(std::string_view topic_name, const ibmw::executor::ExecutorHandle& exe, TCallback&& callback, ibmw::common::util::SourceLocation loc) {
  auto s = sub(loc).Init<T>(topic_name);
  sub(loc).SubscribeOn(s, exe, std::forward<TCallback>(callback));
  return s;
}

// subscriber subscribe inline
template <class T, concepts::SupportedSubscriber<T> TCallback>
void OpSub::SubscribeInline(const res::Subscriber<T>& ch, TCallback callback) {
  DoOnMessage(ch, std::move(callback), {});
}

// subscriber subscribe on
template <class T, concepts::SupportedSubscriber<T> TCallback>
void OpSub::SubscribeOn(const res::Subscriber<T>& ch, ibmw::executor::ExecutorHandle exe, TCallback callback) {
  DoOnMessage(ch, std::move(callback), std::move(exe));
}

namespace details {
// User callbacks run inside noexcept C-ABI invokers: an escaping exception
// would std::terminate the driver thread, so log and drop it here instead.
template <class F>
inline void InvokeSubscriberCallback(Context& ctx, F&& f) noexcept {
  try {
    std::forward<F>(f)();
  } catch (const std::exception& e) {
    ctx.log().Error("Subscriber callback threw: {}", e.what());
  } catch (...) {
    ctx.log().Error("Subscriber callback threw an unknown exception.");
  }
}
}  // namespace details

// subscriber subscribe
template <class T, concepts::SupportedSubscriber<T> TCallback>
void OpSub::DoOnMessage(const res::Subscriber<T>& ch, TCallback callback, ibmw::executor::ExecutorHandle exe) {
  auto& transport_ctx = ctx_.GetTransportResource(ch, loc_);
  auto& sub_fn = std::any_cast<typename Context::SubscribeFunction<T>&>(transport_ctx.sub_f);
  auto ctx = details::GetCurrentContext();
  const bool ok = sub_fn(transport_ctx.sub,
                         StandardizeSubscriber<T>(std::move(callback)),
                         std::move(exe));
  MW_ASSERT_LOC(loc_, ok, "Subscribe [{}] failed.", ch.GetName());
}

// subscriber subscribe function
template <class T>
Context::SubscribeFunction<T> OpSub::CreateSubscribeFunction() {
  return [](ibmw::transport::ReceiverRef subscriber,
            typename Context::TransportCallback<T> cb,
            ibmw::executor::ExecutorHandle exe) {
    return RawOnMessage<T>(subscriber, std::move(exe), std::move(cb));
  };
}

template <class T, class F>
bool OpSub::RawOnMessage(
    ibmw::transport::ReceiverRef subscriber,
    ibmw::executor::ExecutorHandle exe,
    F&& callback) {
  auto type_support = ibmw::GetMessageTypeSupport<T>();

  auto cb_ptr = std::make_shared<typename Context::TransportCallback<T>>(std::forward<F>(callback));
  auto ctx = details::GetCurrentContext();

  if (!exe) {
    return subscriber.OnMessage(
        type_support,
        [cb_ptr, ctx_ptr = ctx->shared_from_this()](
            const ibmw_transport_context_base_t* chn_ctx_ptr,
            const void* msg_ptr,
            ibmw_function_base_t* release_callback_base) mutable {
          transport::SubscriberReleaseCallback release_callback(release_callback_base);

          // Save and restore the caller's thread-local context around
          // inline dispatch so we don't clobber the publisher's context
          // (on every exit path, including a throwing user callback).
          details::ScopedThreadContextRestore restore_ctx;
          ctx_ptr->LetMe();

          if (!ctx_ptr->Running() || ctx_ptr->GetSubState() != context::TransportState::kOn) [[unlikely]] {
            ctx_ptr->log().Trace("Subscriber is not ready.");
            if (release_callback) release_callback();
            return;
          }
          details::InvokeSubscriberCallback(*ctx_ptr, [&] {
            (*cb_ptr)(
                ibmw::transport::ContextRef(chn_ctx_ptr),
                std::shared_ptr<const T>(
                    static_cast<const T*>(msg_ptr),
                    [release_callback{std::move(release_callback)}](const T*) { release_callback(); }));
          });
        });
  }

  return subscriber.OnMessage(
      type_support,
      [ctx_ptr = ctx->shared_from_this(), exeref = std::move(exe), cb_ptr = std::move(cb_ptr)](
          const ibmw_transport_context_base_t* chn_ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) mutable {
        // Take ownership of the driver buffer first so the early return below releases it.
        transport::SubscriberReleaseCallback release_callback(release_callback_base);
        if (!ctx_ptr->Running() || ctx_ptr->GetSubState() != context::TransportState::kOn) [[unlikely]] {
          ctx_ptr->log().Trace("Subscriber is not ready.");
          if (release_callback) release_callback();
          return;
        }

        auto msg = std::shared_ptr<const T>(
            static_cast<const T*>(msg_ptr),
            [release_callback{std::move(release_callback)}](const T*) { release_callback(); });
        exeref.Dispatch([cb_ptr, ctx_ptr, chn_ctx_ptr, msg = std::move(msg)]() mutable {
          ctx_ptr->LetMe();
          if (!ctx_ptr->Running() || ctx_ptr->GetSubState() != context::TransportState::kOn) [[unlikely]] {
            ctx_ptr->log().Trace("Subscriber is not ready.");
            return;
          }
          details::InvokeSubscriberCallback(*ctx_ptr, [&] {
            (*cb_ptr)(ibmw::transport::ContextRef(chn_ctx_ptr), std::move(msg));
          });
        });
      });
}

template <class T, concepts::SupportedSubscriber<T> F>
auto OpSub::StandardizeSubscriber(F&& cb) {
  using Callback = typename Context::TransportCallback<T>;

  if constexpr (concepts::SubscriberFunctionWithCtx<F, T>) {
    return Callback(std::forward<F>(cb));
  } else if constexpr (concepts::SubscriberFunctionDerefWithCtx<F, T>) {
    return Callback([callback = std::forward<F>(cb)](ibmw::transport::ContextRef ctx, std::shared_ptr<const T> msg) {
      callback(ctx, *msg);
    });
  } else if constexpr (concepts::SubscriberFunction<F, T>) {
    return Callback([callback = std::forward<F>(cb)](ibmw::transport::ContextRef, std::shared_ptr<const T> msg) {
      callback(std::move(msg));
    });
  } else if constexpr (concepts::SubscriberFunctionDeref<F, T>) {
    return Callback([callback = std::forward<F>(cb)](ibmw::transport::ContextRef, std::shared_ptr<const T> msg) {
      callback(*msg);
    });
  } else {
    static_assert(sizeof(F) == 0, "Unsupported subscriber callback type.");
  }
}

// Log
template <class... Args>
inline void OpLog::Log(std::uint32_t level, fmt::format_string<Args...> fmt, Args&&... args) {
  const auto& cur_lgr = ctx_.GetLogger();
  if (level >= cur_lgr.GetLogLevel()) {
    std::string log_str = ::ibmw_fmt::format(fmt, std::forward<Args>(args)...);
    cur_lgr.Log(level, loc_.line(), loc_.file_name(), loc_.function_name(), log_str.c_str(), log_str.size());
  }
}

inline OpLog Log(ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) {
  return ibmw::context::details::GetCurrentContext(loc)->log(loc);
}

// Context CreateClient for service client class
template <class TServiceClient>
inline TServiceClient Context::CreateClient(std::string_view service_name, ibmw::common::util::SourceLocation loc) {
  TServiceClient client;
  client.Init(shared_from_this(), std::string(service_name));
  return client;
}

template <class Q, class P, concepts::RawClient<Q, P> TClient>
res::Client<Q, P> OpCli::Init(std::string_view service_name, TClient&& client) {
  RpcResource rpc_ctx;

  rpc_ctx.call_f = Context::Client<Q, P>(
      [client = std::move(client)](ibmw::rpc::ContextRef ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
        co_return co_await client(ctx, q, p);
      });

  rpc_ctx.func_name = std::string(service_name);

  const size_t idx = ctx_.rpc_resources_.emplace_back(std::move(rpc_ctx));

  res::Client<Q, P> res;
  res.name_ = std::string(service_name);
  res.idx_ = idx;
  res.context_id_ = ctx_.id_;

  return res;
}

template <class Q, class P>
co::Task<ibmw::rpc::Status> OpCli::Call(const res::Service<Q, P>& srv, const Q& q, P& p) {
  return Call(srv, ibmw::rpc::ContextRef(), q, p);
}

template <class Q, class P>
co::Task<ibmw::rpc::Status> OpCli::Call(const res::Service<Q, P>& srv, ibmw::rpc::ContextRef ctx, const Q& q, P& p) {
  details::GetCurrentContext(loc_)->LetMe();
  auto& rpc_ctx = ctx_.GetRpcResource(srv, loc_);
  co_return co_await std::any_cast<std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)>&>(rpc_ctx.call_f)(ctx, q, p);
}

template <class TServiceServer>
std::shared_ptr<TServiceServer> Context::CreateServer(std::string_view service_name, ibmw::common::util::SourceLocation loc) {
  auto server = std::make_shared<TServiceServer>();
  server->Init(shared_from_this(), std::string(service_name));
  return server;
}

// OpSrv::Init implementation
template <class Q, class P>
res::Service<Q, P> OpSrv::Init(std::string_view service_name) {
  RpcResource rpc_ctx;
  rpc_ctx.func_name = std::string(service_name);

  const size_t idx = ctx_.rpc_resources_.emplace_back(std::move(rpc_ctx));

  res::Service<Q, P> res;
  res.name_ = std::string(service_name);
  res.idx_ = idx;
  res.context_id_ = ctx_.id_;

  return res;
}

// OpSrv::ServeInline implementation
template <class Q, class P, concepts::SupportedServer<Q, P> TServer>
void OpSrv::ServeInline(const res::Service<Q, P>& srv, TServer server) {
  auto& rpc_ctx = ctx_.GetRpcResource(srv, loc_);

  auto standardized_server = StandardizeServer<Q, P, TServer>(std::move(server));
  rpc_ctx.serve_f = Context::Server<Q, P>(
      [server_func = std::move(standardized_server), ctx_ptr = ctx_.weak_from_this()](
          ibmw::rpc::ContextRef rpc_ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
        if (auto ctx = ctx_ptr.lock()) {
          ctx->LetMe();
          co_return co_await server_func(rpc_ctx, q, p);
        }
        co_return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_READY);
      });
}

// OpSrv::ServeOn implementation
template <class Q, class P, concepts::SupportedServer<Q, P> TServer>
void OpSrv::ServeOn(const res::Service<Q, P>& srv, ibmw::executor::ExecutorHandle exe, TServer server) {
  auto& rpc_ctx = ctx_.GetRpcResource(srv, loc_);

  auto standardized_server = StandardizeServer<Q, P, TServer>(std::move(server));
  auto ctx_ptr = ctx_.shared_from_this();

  rpc_ctx.serve_f = Context::Server<Q, P>(
      [server_func = std::move(standardized_server), exe = std::move(exe), ctx_ptr](
          ibmw::rpc::ContextRef ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
        co_await ibmw::co::Schedule(ibmw::co::IbmwExecutor(exe));
        ctx_ptr->LetMe();
        co_return co_await server_func(ctx, q, p);
      });
}

// OpSrv::Serving implementation
template <class Q, class P>
ibmw::co::Task<ibmw::rpc::Status> OpSrv::Serving(const res::Service<Q, P>& srv, ibmw::rpc::ContextRef ctx, const Q& q, P& p) {
  details::GetCurrentContext(loc_)->LetMe();
  auto& rpc_ctx = ctx_.GetRpcResource(srv, loc_);

  if (!rpc_ctx.serve_f.has_value()) {
    co_return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);
  }

  if (!ctx_.Running() || ctx_.GetSrvState() != context::RpcState::kOn) [[unlikely]] {
    ctx_.log().Trace("Server is not ready to serve.");
    co_return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_READY);
  }

  co_return co_await std::any_cast<Context::Server<Q, P>&>(rpc_ctx.serve_f)(ctx, q, p);
}

// OpSrv::StandardizeServer implementation
template <class Q, class P, concepts::SupportedServer<Q, P> Func>
constexpr auto OpSrv::StandardizeServer(Func&& cb) {
  using namespace concepts;
  using ServerFunc = std::function<ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q&, P&)>;

  // Handle coroutine types (already return Task<Status>)
  if constexpr (ServerCoroutineWithCtx<decltype(cb), Q, P>) {
    return ServerFunc(std::forward<Func>(cb));
  } else if constexpr (ServerCoroutine<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      return callback(q, p);
    });
  } else if constexpr (ServerCoroutineReturnVoidWithCtx<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      co_await callback(ctx, q, p);
      co_return ibmw::rpc::Status();
    });
  } else if constexpr (ServerCoroutineReturnVoid<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      co_await callback(q, p);
      co_return ibmw::rpc::Status();
    });
  }
  // Handle synchronous function types (wrap in coroutine)
  else if constexpr (ServerFunctionWithCtx<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      co_return callback(ctx, q, p);
    });
  } else if constexpr (ServerFunctionReturnVoidWithCtx<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef ctx, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      callback(ctx, q, p);
      co_return ibmw::rpc::Status();
    });
  } else if constexpr (ServerFunction<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      co_return callback(q, p);
    });
  } else if constexpr (ServerFunctionReturnVoid<decltype(cb), Q, P>) {
    return ServerFunc([callback = std::forward<Func>(cb)](
                          ibmw::rpc::ContextRef, const Q& q, P& p) -> ibmw::co::Task<ibmw::rpc::Status> {
      callback(q, p);
      co_return ibmw::rpc::Status();
    });
  } else {
    static_assert(sizeof(decltype(cb)) == 0, "Unsupported server callback type.");
  }
}

}  // namespace ibmw::context

namespace ibmw::context::res {

template <class T>
inline void Publisher<T>::Send(const T& msg, ibmw::common::util::SourceLocation loc) const {
  ibmw::context::details::GetCurrentContext(loc)->pub().Send(*this, msg);
}

template <class T>
inline void Publisher<T>::Send(ibmw::transport::ContextRef ch_ctx, const T& msg, ibmw::common::util::SourceLocation loc) const {
  ibmw::context::details::GetCurrentContext(loc)->pub().Send(*this, ch_ctx, msg);
}

template <class T>
template <concepts::SupportedSubscriber<T> TCallback>
inline Subscriber<T> Subscriber<T>::SubscribeOn(const ibmw::executor::ExecutorHandle& exe, TCallback callback, ibmw::common::util::SourceLocation loc) const {
  ibmw::context::details::GetCurrentContext(loc)->sub().SubscribeOn(*this, exe, std::move(callback));
  return *this;
}

template <class T>
template <concepts::SupportedSubscriber<T> TCallback>
inline Subscriber<T> Subscriber<T>::SubscribeInline(TCallback callback, ibmw::common::util::SourceLocation loc) const {
  ibmw::context::details::GetCurrentContext(loc)->sub().SubscribeInline(*this, std::move(callback));
  return *this;
}

// Implementation of Client::Call method
template <class Q, class P>
inline co::Task<ibmw::rpc::Status> Client<Q, P>::Call(
    const Q& q, P& p, ibmw::common::util::SourceLocation loc) const {
  return Call(ibmw::rpc::ContextRef(), q, p, loc);
}

template <class Q, class P>
inline co::Task<ibmw::rpc::Status> Client<Q, P>::Call(
    ibmw::rpc::ContextRef ctx, const Q& q, P& p, ibmw::common::util::SourceLocation loc) const {
  auto ctx_ptr = ibmw::context::details::GetCurrentContext(loc);
  if (ctx_ptr->GetCliState() != context::RpcState::kOn) [[unlikely]] {
    co_return ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_NOT_READY);
  }
  co_return co_await ctx_ptr->cli().Call(*this, ctx, q, p);
}

// Implementation of Server methods
template <class Q, class P>
template <typename TServer>
inline void Server<Q, P>::ServeInline(TServer server, ibmw::common::util::SourceLocation loc) const {
  auto ctx_ptr = ibmw::context::details::GetCurrentContext(loc);
  ctx_ptr->srv(loc).ServeInline(*this, std::move(server));
}

template <class Q, class P>
template <typename TServer>
inline void Server<Q, P>::ServeOn(
    const ibmw::executor::ExecutorHandle& exe, TServer server, ibmw::common::util::SourceLocation loc) const {
  auto ctx_ptr = ibmw::context::details::GetCurrentContext(loc);
  ctx_ptr->srv(loc).ServeOn(*this, exe, std::move(server));
}

}  // namespace ibmw::context::res

#ifdef IBMW_CONTEXT_DEFINE_GLOBAL_LOGGER_HANDLE

inline ibmw::logger::LoggerRef GetLogger() {
  if (!ibmw::context::details::g_thread_ctx.ctx_ptr.expired()) {
    return ibmw::context::details::GetCurrentContext()->GetRawRef().GetLogger();
  }
  return ibmw::logger::GetSimpleLoggerRef();
}

#endif
