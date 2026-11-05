// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)

namespace my_project {

_IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl::_IbmwJsonRpcServerJsonRpcServerModuleComputeServiceImpl(JsonRpcServerModule* owner)
    : ibmw::rpc::ServiceBase("json", "my_project.ComputeService"), owner_(owner) {
    ibmw::json::rpc::RegisterServiceFunc<ComputeRequest, ComputeResponse>(
        *this, "Compute",
        [this](const ComputeRequest& req, ComputeResponse& rsp) -> uint32_t {
          return owner_->HandleCompute(req, rsp);
        });
}

uint32_t JsonRpcServerModule::HandleCompute(const ComputeRequest& req, ComputeResponse& rsp) {
  (void)req;
  (void)rsp;
  // IBMW_RPC_SERVER_HANDLER_BODY_COMPUTE_BEGIN
  rsp.code = 0;
  rsp.msg = "OK";
  if (req.operation == "add") {
    rsp.result = req.operand_a + req.operand_b;
  } else if (req.operation == "multiply") {
    rsp.result = req.operand_a * req.operand_b;
  } else if (req.operation == "subtract") {
    rsp.result = req.operand_a - req.operand_b;
  } else {
    rsp.result = 0.0;
    rsp.code = 1;
    rsp.msg = "UNKNOWN_OPERATION";
  }
  return 0;
  // IBMW_RPC_SERVER_HANDLER_BODY_COMPUTE_END
  return 0;
}

}  // namespace my_project
