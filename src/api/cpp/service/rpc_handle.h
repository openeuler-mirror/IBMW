// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <coroutine>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "c/service/rpc_handle_base.h"
#include "cpp/co/thread_ctx_scope.h"
#include "cpp/context/details/thread_context.h"
#include "cpp/service/rpc_co_filter.h"
#include "cpp/util/function.h"
#include "cpp/util/string.h"

#if defined(_MSC_VER) && !defined(__clang__)
  #define IBMW_RPC_AWAIT_SUSPEND_NOINLINE __declspec(noinline)
#else
  #define IBMW_RPC_AWAIT_SUSPEND_NOINLINE __attribute__((noinline))
#endif

namespace ibmw::rpc {

using ServiceCallback = ibmw::util::Function<ibmw_function_service_callback_ops_t>;
using ClientCallback = ibmw::util::Function<ibmw_function_client_callback_ops_t>;
using ServiceFunc = ibmw::util::Function<ibmw_function_service_func_ops_t>;

inline std::string GetFullFuncName(
    std::string_view rpc_type, std::string_view service_name, std::string_view func_name) {
  std::string full_name;

  full_name.reserve(rpc_type.size() + service_name.size() + func_name.size() + 3);
  full_name.append(rpc_type).append(":/").append(service_name).append("/").append(func_name);

  return full_name;
}

inline std::string_view GetFuncNameWithoutPrefix(std::string_view func_name) {
  auto pos = func_name.find(":/");
  if (pos != std::string_view::npos)
    return func_name.substr(pos + 1);

  return func_name;
}

class ServiceBase {
  friend class ServiceRef;

 public:
  ServiceBase(std::string_view rpc_type, std::string_view service_name)
      : rpc_type_(rpc_type), service_name_(service_name) {}
  virtual ~ServiceBase() = default;

  ServiceBase(const ServiceBase&) = delete;
  ServiceBase& operator=(const ServiceBase&) = delete;

  void SetServiceName(std::string_view service_name) {
    service_name_ = service_name;
  }

  std::string_view RpcType() const {
    return rpc_type_;
  }

  std::string_view ServiceName() const {
    return service_name_;
  }

  void RegisterServiceFunc(
      std::string_view func_name,
      const void* custom_type_support_ptr,
      const ibmw_type_support_base_t* req_type_support,
      const ibmw_type_support_base_t* rsp_type_support,
      ibmw::rpc::ServiceFunc&& service_func) {
    service_func_wrapper_vec_.emplace_back(
        ServiceFuncWrapper{
            .func_name = std::string(func_name),
            .custom_type_support_ptr = custom_type_support_ptr,
            .req_type_support = req_type_support,
            .rsp_type_support = rsp_type_support,
            .service_func = std::move(service_func)});
  }

 protected:
  std::string rpc_type_;
  std::string service_name_;

  struct ServiceFuncWrapper {
    std::string func_name;
    const void* custom_type_support_ptr;
    const ibmw_type_support_base_t* req_type_support;
    const ibmw_type_support_base_t* rsp_type_support;
    ibmw::rpc::ServiceFunc service_func;
  };

  std::vector<ServiceFuncWrapper> service_func_wrapper_vec_;
};

class CoServiceBase : public ServiceBase {
 public:
  CoServiceBase(std::string_view rpc_type, std::string_view service_name)
      : ServiceBase(rpc_type, service_name) {}
  virtual ~CoServiceBase() = default;

  template <typename T>
    requires std::constructible_from<CoRpcFilter, T>
  void RegisterFilter(T&& filter) {
    filter_mgr_.RegisterFilter((T &&) filter);
  }

  auto& GetFilterBus() { return filter_mgr_; }

 protected:
  CoFilterBus filter_mgr_;
};

class ServiceRef {
 public:
  ServiceRef() = default;
  explicit ServiceRef(const ibmw_service_handle_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ServiceRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_service_handle_base_t* NativeHandle() const { return base_ptr_; }

  /**
   * @brief Register service
   *
   * @param service_ptr
   * @return Register result
   */
  bool RegisterService(ServiceBase* service_ptr) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    MW_ASSERT(service_ptr, "Service pointer is null.");

    bool ret = true;

    for (auto& item : service_ptr->service_func_wrapper_vec_) {
      std::string full_func_name = GetFullFuncName(
          service_ptr->rpc_type_, service_ptr->service_name_, item.func_name);

      ret &= base_ptr_->register_service_func(
          base_ptr_->ctx,
          ibmw::util::ToIbmwStringView(full_func_name),
          item.custom_type_support_ptr,
          item.req_type_support,
          item.rsp_type_support,
          item.service_func.NativeHandle());
    }

    return ret;
  }

  /**
   * @brief Register service with specific name
   *
   * @param service_name
   * @param service_ptr
   * @return Register result
   */
  bool RegisterService(std::string_view service_name, ServiceBase* service_ptr) {
    MW_ASSERT(service_ptr, "Service pointer is null.");

    service_ptr->SetServiceName(service_name);

    return RegisterService(service_ptr);
  }

  /**
   * @brief Register client func
   *
   * @param full_func_name
   * @param custom_type_support_ptr
   * @param req_type_support
   * @param rsp_type_support
   * @return Register result
   */
  bool RegisterClientFunc(
      std::string_view full_func_name,
      const void* custom_type_support_ptr,
      const ibmw_type_support_base_t* req_type_support,
      const ibmw_type_support_base_t* rsp_type_support) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->register_client_func(
        base_ptr_->ctx,
        ibmw::util::ToIbmwStringView(full_func_name),
        custom_type_support_ptr,
        req_type_support,
        rsp_type_support);
  }

  bool RegisterClientFunc(
      std::string_view rpc_type,
      std::string_view service_name,
      std::string_view func_name,
      const void* custom_type_support_ptr,
      const ibmw_type_support_base_t* req_type_support,
      const ibmw_type_support_base_t* rsp_type_support) {
    return RegisterClientFunc(
        GetFullFuncName(rpc_type, service_name, func_name),
        custom_type_support_ptr, req_type_support, rsp_type_support);
  }

  void Call(
      std::string_view full_func_name,
      ContextRef ctx_ref,
      const void* req_ptr,
      void* rsp_ptr,
      ibmw::rpc::ClientCallback&& callback) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->call(
        base_ptr_->ctx,
        ibmw::util::ToIbmwStringView(full_func_name),
        ctx_ref.NativeHandle(),
        req_ptr,
        rsp_ptr,
        callback.NativeHandle());
  }

  void Call(
      std::string_view rpc_type,
      std::string_view service_name,
      std::string_view func_name,
      ContextRef ctx_ref,
      const void* req_ptr,
      void* rsp_ptr,
      ibmw::rpc::ClientCallback&& callback) {
    Call(
        GetFullFuncName(rpc_type, service_name, func_name),
        ctx_ref, req_ptr, rsp_ptr, std::move(callback));
  }

  void MergeServerContextToClientContext(
      const ContextRef server_ctx_ref, ContextRef client_ctx_ref) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->merge_server_context_to_client_context(
        base_ptr_->ctx,
        server_ctx_ref.NativeHandle(),
        client_ctx_ref.NativeHandle());
  }

 private:
  const ibmw_service_handle_base_t* base_ptr_ = nullptr;
};

class AccessBase {
 public:
  AccessBase(ServiceRef service_ref, std::string_view rpc_type, std::string_view service_name)
      : service_ref_(service_ref), rpc_type_(rpc_type), service_name_(service_name) {}
  virtual ~AccessBase() = default;

  AccessBase(const AccessBase&) = delete;
  AccessBase& operator=(const AccessBase&) = delete;

  void SetServiceName(std::string_view service_name) {
    service_name_ = service_name;
  }

  std::string_view RpcType() const {
    return rpc_type_;
  }

  std::string_view ServiceName() const {
    return service_name_;
  }

  std::shared_ptr<Context> NewContextSharedPtr(ContextRef ctx_ref = ContextRef()) const {
    auto result_ctx = default_ctx_ptr_
                          ? std::make_shared<Context>(*default_ctx_ptr_)
                          : std::make_shared<Context>();
    if (ctx_ref) {
      service_ref_.MergeServerContextToClientContext(ctx_ref, result_ctx);
    }

    return result_ctx;
  }

  void SetDefaultContextSharedPtr(const std::shared_ptr<Context>& ctx_ptr) {
    default_ctx_ptr_ = ctx_ptr;
  }

  std::shared_ptr<Context> GetDefaultContextSharedPtr() const {
    return default_ctx_ptr_;
  }

 protected:
  ServiceRef service_ref_;
  std::string rpc_type_;
  std::string service_name_;
  std::shared_ptr<Context> default_ctx_ptr_;
};

namespace details {
// Shared + set-once completion for blocking RPC calls: a provider that completes
// twice (or after the caller has returned) must neither terminate in the
// noexcept invoker nor touch a dead stack promise.
struct SyncCallResult {
  std::promise<ibmw::rpc::Status> promise;
  std::future<ibmw::rpc::Status> future = promise.get_future();
  std::atomic_flag done = ATOMIC_FLAG_INIT;

  void Set(ibmw::rpc::Status st) {
    if (!done.test_and_set(std::memory_order_acq_rel)) promise.set_value(st);
  }
  ibmw::rpc::Status Get() { return future.get(); }
};
}  // namespace details

class SyncAccessBase : public AccessBase {
 public:
  SyncAccessBase(ServiceRef service_ref, std::string_view rpc_type, std::string_view service_name)
      : AccessBase(service_ref, rpc_type, service_name) {}
  virtual ~SyncAccessBase() = default;

 protected:
  template <class ReqType, class RspType>
  ibmw::rpc::Status Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      const ReqType& req,
      RspType& rsp) {
    auto result = std::make_shared<details::SyncCallResult>();
    auto on_done = [result](uint32_t code) { result->Set(ibmw::rpc::Status(code)); };

    if (ctx_ref) {
      ctx_ref.SetFunctionName(full_func_name);
      service_ref_.Call(full_func_name, ctx_ref, &req, &rsp, std::move(on_done));
      return result->Get();
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetFunctionName(full_func_name);
    service_ref_.Call(full_func_name, *ctx_ptr, &req, &rsp, std::move(on_done));
    return result->Get();
  }

};

// NOTE: req/rsp are passed to the provider by address. With the reference
// overloads of AsyncAccessBase and FutureAccessBase the caller must keep both
// alive until the callback runs / the future becomes ready. The shared_ptr
// overloads keep them alive until the provider drops its completion callback;
// a null req or rsp completes with IBMW_RPC_STATUS_CLI_UNKNOWN.
class AsyncAccessBase : public AccessBase {
 public:
  AsyncAccessBase(ServiceRef service_ref, std::string_view rpc_type, std::string_view service_name)
      : AccessBase(service_ref, rpc_type, service_name) {}
  virtual ~AsyncAccessBase() = default;

 protected:
  template <class ReqType, class RspType>
  void Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      const ReqType& req,
      RspType& rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) {
    if (ctx_ref) {
      ctx_ref.SetFunctionName(full_func_name);

      service_ref_.Call(
          full_func_name, ctx_ref, &req, &rsp,
          [callback{std::move(callback)}](uint32_t code) {
            callback(ibmw::rpc::Status(code));
          });

      return;
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetFunctionName(full_func_name);

    service_ref_.Call(
        full_func_name, *ctx_ptr, &req, &rsp,
        [ctx_ptr, callback{std::move(callback)}](uint32_t code) {
          callback(ibmw::rpc::Status(code));
        });
  }

  template <class ReqType, class RspType>
  void Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      std::shared_ptr<const ReqType> req,
      std::shared_ptr<RspType> rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) {
    if (!req || !rsp) [[unlikely]] {
      callback(ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
      return;
    }
    const ReqType& req_ref = *req;
    RspType& rsp_ref = *rsp;
    Call(full_func_name, ctx_ref, req_ref, rsp_ref,
         [req{std::move(req)}, rsp{std::move(rsp)}, callback{std::move(callback)}](ibmw::rpc::Status status) {
           callback(status);
         });
  }
};

class FutureAccessBase : public AccessBase {
 public:
  FutureAccessBase(ServiceRef service_ref, std::string_view rpc_type, std::string_view service_name)
      : AccessBase(service_ref, rpc_type, service_name) {}
  virtual ~FutureAccessBase() = default;

 protected:
  template <class ReqType, class RspType>
  std::future<ibmw::rpc::Status> Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      const ReqType& req,
      RspType& rsp) {
    std::promise<ibmw::rpc::Status> status_promise;
    std::future<ibmw::rpc::Status> status_future = status_promise.get_future();

    if (ctx_ref) {
      ctx_ref.SetFunctionName(full_func_name);

      service_ref_.Call(
          full_func_name, ctx_ref, &req, &rsp,
          [status_promise{std::move(status_promise)}, done = false](uint32_t code) mutable {
            if (std::exchange(done, true)) return;  // ignore duplicate completion
            status_promise.set_value(ibmw::rpc::Status(code));
          });

      return status_future;
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetFunctionName(full_func_name);

    service_ref_.Call(
        full_func_name, *ctx_ptr, &req, &rsp,
        [ctx_ptr, status_promise{std::move(status_promise)}, done = false](uint32_t code) mutable {
          if (std::exchange(done, true)) return;  // ignore duplicate completion
          status_promise.set_value(ibmw::rpc::Status(code));
        });

    return status_future;
  }

  template <class ReqType, class RspType>
  std::future<ibmw::rpc::Status> Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      std::shared_ptr<const ReqType> req,
      std::shared_ptr<RspType> rsp) {
    std::promise<ibmw::rpc::Status> status_promise;
    std::future<ibmw::rpc::Status> status_future = status_promise.get_future();
    if (!req || !rsp) [[unlikely]] {
      status_promise.set_value(ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
      return status_future;
    }

    std::shared_ptr<Context> ctx_ptr;
    if (ctx_ref) {
      ctx_ref.SetFunctionName(full_func_name);
    } else {
      ctx_ptr = NewContextSharedPtr();
      ctx_ptr->SetFunctionName(full_func_name);
      ctx_ref = ibmw::rpc::ContextRef(*ctx_ptr);
    }

    const void* req_ptr = req.get();
    void* rsp_ptr = rsp.get();
    service_ref_.Call(
        full_func_name, ctx_ref, req_ptr, rsp_ptr,
        [ctx_ptr{std::move(ctx_ptr)}, req{std::move(req)}, rsp{std::move(rsp)},
         status_promise{std::move(status_promise)}, done = false](uint32_t code) mutable {
          if (std::exchange(done, true)) return;  // ignore duplicate completion
          status_promise.set_value(ibmw::rpc::Status(code));
        });

    return status_future;
  }
};

class CoAccessBase : public AccessBase {
 public:
  CoAccessBase(ServiceRef service_ref, std::string_view rpc_type, std::string_view service_name)
      : AccessBase(service_ref, rpc_type, service_name) {}
  virtual ~CoAccessBase() = default;

  template <typename T>
    requires std::constructible_from<CoRpcFilter, T>
  void RegisterFilter(T&& filter) {
    filter_mgr_.RegisterFilter((T &&) filter);
  }

  auto& GetFilterBus() { return filter_mgr_; }

 protected:
  // Deliberately not an aggregate. GCC 11/12 destroy the members of an
  // aggregate-initialized temporary twice when it is the operand of co_await
  // (`co_await CallAwaitable{.x = ...}`): the shared_ptr members were released
  // twice, a heap use-after-free on every coroutine RPC call. A user-declared
  // constructor makes `co_await CallAwaitable(...)` / `{...}` safe on every
  // compiler (see selfdev/coro/README.md, "Rules for awaiters").
  struct CallAwaitable {
    CallAwaitable(ibmw::rpc::ServiceRef service_ref_in,
                  std::shared_ptr<const std::string> full_func_name_in,
                  ibmw::rpc::ContextRef ctx_ref_in,
                  std::shared_ptr<Context> ctx_owner_in,
                  ibmw::executor::ExecutorHandle resume_exe_in,
                  const void* req_ptr_in,
                  void* rsp_ptr_in)
        : service_ref(std::move(service_ref_in)),
          full_func_name(std::move(full_func_name_in)),
          ctx_ref(ctx_ref_in),
          ctx_owner(std::move(ctx_owner_in)),
          resume_exe(std::move(resume_exe_in)),
          req_ptr(req_ptr_in),
          rsp_ptr(rsp_ptr_in) {}

    ibmw::rpc::ServiceRef service_ref;
    std::shared_ptr<const std::string> full_func_name;
    ibmw::rpc::ContextRef ctx_ref;
    std::shared_ptr<Context> ctx_owner;
    ibmw::executor::ExecutorHandle resume_exe;
    const void* req_ptr;
    void* rsp_ptr;

    ibmw::rpc::Status status;

    // Owned by the completion callback. Resumes the caller exactly once: with
    // the provider's code, or with CLI_DRIVER_INTERNAL_ERROR when every copy
    // of the callback is destroyed without being called (a lost answer would
    // otherwise leak the suspended coroutine frame).
    struct Resumer {
      Resumer(CallAwaitable* awaitable_in, std::coroutine_handle<> handle_in,
              ibmw::executor::ExecutorHandle resume_exe_in)
          : awaitable(awaitable_in), handle(handle_in), resume_exe(std::move(resume_exe_in)) {}

      CallAwaitable* awaitable;
      std::coroutine_handle<> handle;
      ibmw::executor::ExecutorHandle resume_exe;
      std::atomic<bool> completed{false};

      void Complete(uint32_t code) {
        if (completed.exchange(true, std::memory_order_acq_rel)) return;
        // The coroutine is suspended until the dispatched task resumes it.
        awaitable->status = ibmw::rpc::Status(code);
        resume_exe.Dispatch([handle{handle}, exe{resume_exe}]() {
          // Resume with g_thread_ctx.exe set to the calling executor, as
          // co::IbmwExecutor's resumers do, so code after co_await (for
          // example the next RPC call, which takes its resume executor from
          // there) sees it; the worker's own context is restored afterwards.
          ibmw::co::details::ThreadCtxScope scope(exe);
          handle.resume();
        });
      }

      ~Resumer() {
        try {
          Complete(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR);
        } catch (...) {
        }
      }
    };

    // Off an executor thread there is nowhere to resume: fail without suspending.
    bool await_ready() noexcept {
      if (resume_exe) [[likely]] return false;
      status = ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_INVALID_CONTEXT);
      return true;
    }

    // Once Call() has the callback, the coroutine may be resumed and its
    // frame (which holds *this) destroyed on another thread. So everything
    // used from there on is a local copy, and the function is kept out of
    // line: Clang (before 19) may otherwise place the locals of an inlined
    // await_suspend in the coroutine frame, where their destructors would
    // run on freed memory.
    IBMW_RPC_AWAIT_SUSPEND_NOINLINE void await_suspend(std::coroutine_handle<> h) {
      auto service_ref_copy = service_ref;
      auto full_func_name_copy = full_func_name;
      auto ctx_ref_copy = ctx_ref;
      auto req_ptr_copy = req_ptr;
      auto rsp_ptr_copy = rsp_ptr;

      auto resumer = std::make_shared<Resumer>(this, h, resume_exe);
      service_ref_copy.Call(
          *full_func_name_copy, ctx_ref_copy, req_ptr_copy, rsp_ptr_copy,
          [resumer{std::move(resumer)}](uint32_t code) { resumer->Complete(code); });
    }

    auto await_resume() noexcept { return status; }
  };
  // Regression guard for the GCC 11/12 double destruction above: removing the
  // constructor (making it an aggregate again) must not compile.
  static_assert(!std::is_aggregate_v<CallAwaitable>,
                "CallAwaitable must not be an aggregate (GCC co_await double-destroy)");

  struct CallHandle {
    ibmw::rpc::ServiceRef service_ref;
    std::shared_ptr<const std::string> full_func_name;
    std::shared_ptr<Context> ctx_owner;
    ibmw::executor::ExecutorHandle resume_exe;

    ibmw::co::Task<ibmw::rpc::Status> operator()(ibmw::rpc::ContextRef ctx_ref, const void* req_ptr, void* rsp_ptr) {
      auto service_ref_keep = service_ref;
      auto full_func_name_keep = full_func_name;
      auto ctx_owner_keep = ctx_owner;
      auto resume_exe_keep = resume_exe;

      co_return co_await CallAwaitable(
          service_ref_keep, full_func_name_keep, ctx_ref, ctx_owner_keep, resume_exe_keep, req_ptr, rsp_ptr);
    }
  };

  template <class ReqType, class RspType>
  ibmw::co::Task<ibmw::rpc::Status> Call(
      std::string_view full_func_name,
      ibmw::rpc::ContextRef ctx_ref,
      const ReqType& req,
      RspType& rsp) {
    auto owned_full_func_name = std::make_shared<const std::string>(full_func_name);
    std::shared_ptr<Context> ctx_owner;
    ibmw::rpc::ContextRef effective_ctx_ref = ctx_ref;

    if (effective_ctx_ref) {
      effective_ctx_ref.SetFunctionName(full_func_name);
    } else {
      ctx_owner = NewContextSharedPtr();
      ctx_owner->SetFunctionName(full_func_name);
      effective_ctx_ref = *ctx_owner;
    }

    auto h = std::make_shared<ibmw::rpc::CoRpcHandle>(CallHandle{
        .service_ref = service_ref_,
        .full_func_name = owned_full_func_name,
        .ctx_owner = ctx_owner,
        .resume_exe = ibmw::context::details::g_thread_ctx.exe});

    co_return co_await filter_mgr_.InvokeRpc(
        std::move(h), effective_ctx_ref, static_cast<const void*>(&req), static_cast<void*>(&rsp));
  }

 protected:
  CoFilterBus filter_mgr_;
};

template <class AccessType>
bool RegisterClientFunc(ServiceRef service_ref) {
  return AccessType::RegisterClientFunc(service_ref);
}

template <class AccessType>
bool RegisterClientFunc(ServiceRef service_ref, std::string_view service_name) {
  return AccessType::RegisterClientFunc(service_ref, service_name);
}

}  // namespace ibmw::rpc
