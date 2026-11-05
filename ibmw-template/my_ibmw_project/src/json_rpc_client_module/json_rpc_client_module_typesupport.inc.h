// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_rpc_helpers.h"
#include "json_custom_types.h"

namespace my_project {

static constexpr const char* kServiceName = "my_project.ComputeService";

}  // namespace my_project

// Thread-form client loop uses std::this_thread::sleep_for in the
// transport-owned helper emitted in the companion inc.cc.

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                             void RpcLoop();                                                                                                                                                private:                                                                        template <typename SelfT>                                                      friend void _ibmw_json_rpc_client_loop_body_impl(SelfT*);                      auto GetLogger() { return core_.GetLogger(); }                               ibmw::rpc::ServiceRef service_ref_;                                            std::atomic<bool> running_{false};

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                       void name::RpcLoop() {                                                          _ibmw_json_rpc_client_loop_body_impl(this);                                  }
