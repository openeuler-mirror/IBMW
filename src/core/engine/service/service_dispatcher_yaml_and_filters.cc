// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Built-in debug-log RPC filter registration.

#include "engine/service/service_dispatcher.h"
#include "engine/service/service_driver_tools.h"
#include "engine/transport/internal/debug_log_format.h"

namespace ibmw::runtime::core::service {

namespace {
// Pull the framework-internal debug-log format anchor.
// Same value as transport_bus.cc; single point of definition lives in the
// transport/internal header to avoid re-introducing literal coupling.
using ::ibmw::runtime::core::transport::internal::kDebugLogSerializationFormat;
}  // namespace

void ServiceDispatcher::RegisterDebugLogFilter() {
  RegisterClientFilter(
      "debug_log",
      [this](const std::shared_ptr<ServiceMessageFrame>& ptr, FrameworkAsyncRpcHandle&& h) {
        auto buf_ptr = TrySerializeReqWithCache(
            *ptr, kDebugLogSerializationFormat);

        if (buf_ptr) {
          auto req_str = buf_ptr->JoinToString();

          MW_INFO("Service client start new service call. func name: {}, context: {}, req: {}",
                     ptr->info.func_name, ptr->ctx_ref.ToString(), req_str);
        } else {
          MW_INFO("Service client start new service call. func name: {}, context: {}",
                     ptr->info.func_name, ptr->ctx_ref.ToString());
        }

        // Weak capture: a strong one would make the frame own itself and leak
        // it when a driver drops the call.
        ptr->callback =
            [this, frame_weak{std::weak_ptr<ServiceMessageFrame>(ptr)},
             callback{std::move(ptr->callback)}](ibmw::rpc::Status status) {
              auto frame = frame_weak.lock();
              if (!frame) [[unlikely]] {
                callback(status);
                return;
              }
              if (!status.OK()) {
                MW_WARN("Service client get rpc error ret. func name: {}, status: {}",
                           frame->info.func_name, status.ToString());
              } else {
                auto buf_ptr = TrySerializeRspWithCache(
                    *frame, kDebugLogSerializationFormat);

                if (buf_ptr) {
                  auto rsp_str = buf_ptr->JoinToString();

                  MW_INFO("Service client get rpc ret. func name: {}, status: {}, rsp: {}",
                             frame->info.func_name, status.ToString(), rsp_str);
                } else {
                  MW_INFO("Service client get rpc ret. func name: {}, status: {}",
                             frame->info.func_name, status.ToString());
                }
              }

              callback(status);
            };

        h(ptr);
      });

  RegisterServerFilter(
      "debug_log",
      [this](const std::shared_ptr<ServiceMessageFrame>& ptr, FrameworkAsyncRpcHandle&& h) {
        auto buf_ptr = TrySerializeReqWithCache(
            *ptr, kDebugLogSerializationFormat);

        if (buf_ptr) {
          auto req_str = buf_ptr->JoinToString();

          MW_INFO("Service server start new service call. func name: {}, context: {}, req: {}",
                     ptr->info.func_name, ptr->ctx_ref.ToString(), req_str);
        } else {
          MW_INFO("Service server start new service call. func name: {}, context: {}",
                     ptr->info.func_name, ptr->ctx_ref.ToString());
        }

        ptr->callback =
            [this, ptr, callback{std::move(ptr->callback)}](ibmw::rpc::Status status) {
              if (!status.OK()) {
                MW_WARN("Service server get rpc error ret. func name: {}, status: {}",
                           ptr->info.func_name, status.ToString());
              } else {
                auto buf_ptr = TrySerializeRspWithCache(
                    *ptr, kDebugLogSerializationFormat);

                if (buf_ptr) {
                  auto rsp_str = buf_ptr->JoinToString();

                  MW_INFO("Service server get rpc ret. func name: {}, status: {}, rsp: {}",
                             ptr->info.func_name, status.ToString(), rsp_str);
                } else {
                  MW_INFO("Service server get rpc ret. func name: {}, status: {}",
                             ptr->info.func_name, status.ToString());
                }
              }

              callback(status);
            };

        h(ptr);
      });
}

}  // namespace ibmw::runtime::core::service
