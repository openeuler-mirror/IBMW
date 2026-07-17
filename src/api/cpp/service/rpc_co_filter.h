// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>

#include "base/inline_function.h"
#include "cpp/co/task.h"
#include "cpp/service/rpc_context.h"
#include "cpp/service/rpc_status.h"

namespace ibmw::rpc {

using CoRpcHandle = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*)>;
using CoRpcFilter = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*, const CoRpcHandle&)>;
using CoRpcHandlePtr = std::shared_ptr<CoRpcHandle>;
using CoRpcInternalFilter = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*, CoRpcHandlePtr)>;

class CoFilterBus {
 public:
  CoFilterBus()
      : final_filter_([](ContextRef ctx_ref, const void* req, void* rsp, CoRpcHandlePtr h) -> ibmw::co::Task<Status> {
          auto keep_alive = std::move(h);
          co_return co_await (*keep_alive)(ctx_ref, req, rsp);
        }) {}
  ~CoFilterBus() = default;

  CoFilterBus(const CoFilterBus&) = delete;
  CoFilterBus& operator=(const CoFilterBus&) = delete;

  void RegisterFilter(CoRpcFilter&& filter) {
    final_filter_ =
        [final_filter{std::move(final_filter_)}, cur_filter{std::move(filter)}](
            ContextRef ctx_ref, const void* req, void* rsp, CoRpcHandlePtr h) -> ibmw::co::Task<Status> {
      auto next_handle = std::make_shared<CoRpcHandle>(
          [&final_filter, h{std::move(h)}](ContextRef ctx_ref, const void* req, void* rsp) -> ibmw::co::Task<Status> {
            return final_filter(ctx_ref, req, rsp, h);
          });
      co_return co_await cur_filter(
          ctx_ref, req, rsp, *next_handle);
    };
  }

  ibmw::co::Task<Status> InvokeRpc(CoRpcHandlePtr h, ContextRef ctx_ref, const void* req, void* rsp) const {
    return final_filter_(ctx_ref, req, rsp, std::move(h));
  }

  void Clear() {
    final_filter_ = CoRpcInternalFilter();
  }

 private:
  CoRpcInternalFilter final_filter_;
};

}  // namespace ibmw::rpc
