// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/service/rpc_co_filter.h"
#include "normal_rpc_co_server_module/global.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module {

inline co::Task<ibmw::rpc::Status> TimeCostLogServerFilter(
    ibmw::rpc::ContextRef ctx, const void* req_ptr, void* rsp_ptr,
    const ibmw::rpc::CoRpcHandle& next) {
  auto begin_time = std::chrono::steady_clock::now();
  const auto& status = co_await next(ctx, req_ptr, rsp_ptr);
  auto end_time = std::chrono::steady_clock::now();

  MW_INFO("Svr rpc time cost {} us",
             std::chrono::duration_cast<std::chrono::microseconds>(end_time - begin_time).count());

  co_return status;
}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module
