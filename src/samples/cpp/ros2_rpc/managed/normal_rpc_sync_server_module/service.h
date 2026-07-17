// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_sync_server_module {

class RosTestRpcSyncServiceImpl : public example_ros2::srv::RosTestRpcSyncService {
 public:
  RosTestRpcSyncServiceImpl() = default;
  ~RosTestRpcSyncServiceImpl() override = default;

  ibmw::rpc::Status RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_sync_server_module
