// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "RosTestRpc.ibmw_rpc.srv.h"

// Thread-form client loop uses std::this_thread::sleep_for in the
// transport-owned helper emitted in the companion inc.cc.

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                           \
  void RpcLoop();                                                              \
                                                                                  \
 private:                                                                      \
  template <typename SelfT>                                                    \
  friend void _ibmw_ros2_rpc_client_loop_body_impl(SelfT*);                    \
  auto GetLogger() { return core_.GetLogger(); }                             \
  std::shared_ptr<example_ros2::srv::RosTestRpcSyncAccess> access_;                                        \
  std::thread loop_thread_;                                                    \
  std::atomic_bool run_flag_{true};                                          \
  double rpc_frq_{1.0};

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                     \
  void name::RpcLoop() {                                                      \
    _ibmw_ros2_rpc_client_loop_body_impl(this);                                \
  }
