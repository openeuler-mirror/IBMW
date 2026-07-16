// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <memory>

#include "cpp/ibmw_sdk.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "AddTwoInts.ibmw_rpc.dds.h"

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                             ibmw::co::Task<void> RpcLoop();                                                                                                                               private:                                                                        template <typename SelfT>                                                      friend ibmw::co::Task<void> _ibmw_dds_rpc_client_loop_body_impl_rpc_client_module(SelfT*);                            auto GetLogger() const { return ctx_ptr_->GetLogger(); }                     std::shared_ptr<ibmw::context::Context> ctx_ptr_;                              dds_rpc_demo::AddTwoIntsServiceCoClient client_;                                          ibmw::executor::ExecutorHandle time_executor_;                                 ibmw::co::AsyncScope scope_;

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                       ibmw::co::Task<void> name::RpcLoop() {                                          co_await _ibmw_dds_rpc_client_loop_body_impl_rpc_client_module(this);                                                  co_return;                                                                   }
