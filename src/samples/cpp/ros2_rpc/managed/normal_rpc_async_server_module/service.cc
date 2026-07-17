// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_async_server_module/service.h"

#include "normal_rpc_async_server_module/global.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module {

void RosTestRpcAsyncServiceImpl::RosTestRpc(
    ibmw::rpc::ContextRef ctx,
    const example_ros2::srv::RosTestRpc_Request& req,
    example_ros2::srv::RosTestRpc_Response& rsp,
    std::function<void(ibmw::rpc::Status)>&& callback) {
  rsp.code = 123;

  MW_INFO("Get new service call. context: {}\n, req:\n{}\nreturn rsp:\n{}",
             ctx.ToString(), example_ros2::srv::to_yaml(req), example_ros2::srv::to_yaml(rsp));

  callback(ibmw::rpc::Status());
}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module
