// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module {

class RosTestRpcAsyncServiceImpl : public example_ros2::srv::RosTestRpcAsyncService {
 public:
  RosTestRpcAsyncServiceImpl() = default;
  ~RosTestRpcAsyncServiceImpl() override = default;

  void RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) override;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module
