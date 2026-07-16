// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module {

class RosTestRpcServiceImpl : public example_ros2::srv::RosTestRpcCoService {
 public:
  RosTestRpcServiceImpl() = default;
  ~RosTestRpcServiceImpl() override = default;

  co::Task<ibmw::rpc::Status> RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module
