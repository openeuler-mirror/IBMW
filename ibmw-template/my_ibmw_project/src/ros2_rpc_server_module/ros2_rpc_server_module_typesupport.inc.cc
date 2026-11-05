// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

namespace my_project {

template <typename ServerT>
inline ibmw::rpc::Status _ibmw_ros2_rpc_server_marshalling_helper_ros_test_rpc(
    ServerT* self,
    ibmw::rpc::ContextRef ctx,
    const example_ros2::srv::RosTestRpc_Request& req,
    example_ros2::srv::RosTestRpc_Response& rsp) {
  (void)self;
  (void)ctx;
  (void)req;
  (void)rsp;
  // IBMW_RPC_SERVER_HANDLER_BODY_ROS_TEST_RPC_BEGIN
  // Echo the request data size as the response code
  rsp.code = static_cast<int64_t>(req.data.size());
  MW_INFO("ROS2 RPC Server: received {} bytes, responding with code={}",
          req.data.size(), rsp.code);
  return ibmw::rpc::Status();
  // IBMW_RPC_SERVER_HANDLER_BODY_ROS_TEST_RPC_END
}

ibmw::rpc::Status _IbmwRos2RpcServerRos2RpcServerModuleRosTestRpcSyncServiceImpl::RosTestRpc(
    ibmw::rpc::ContextRef ctx,
    const example_ros2::srv::RosTestRpc_Request& req,
    example_ros2::srv::RosTestRpc_Response& rsp) {
  return owner_->HandleRosTestRpc(ctx, req, rsp);
}

}  // namespace my_project

#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)                                  \
  ibmw::rpc::Status name::HandleRosTestRpc(                                     \
      ibmw::rpc::ContextRef ctx,                                              \
      const example_ros2::srv::RosTestRpc_Request& req,                                               \
      example_ros2::srv::RosTestRpc_Response& rsp) {                                                 \
    return _ibmw_ros2_rpc_server_marshalling_helper_ros_test_rpc(this, ctx, req, rsp);                    \
  }
