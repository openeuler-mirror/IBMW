// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "AddTwoInts.ibmw_rpc.dds.h"

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                     do { \
    ctx_ptr_ = std::make_shared<ibmw::context::Context>((core_expr)); \
    ctx_ptr_->LetMe(); \
    try { \
      auto server_handle = ctx_ptr_->CreateServer<dds_rpc_demo::AddTwoIntsServiceCoServer>(); \
    server_handle->AddTwoInts.ServeInline( \
        [this](ibmw::rpc::ContextRef ctx, \
               const dds_rpc_demo::AddTwoInts_Request& req, \
               dds_rpc_demo::AddTwoInts_Response& rsp) { \
          return _ibmw_dds_rpc_server_marshalling_helper_rpc_server_module_add_two_ints(this, ctx, req, rsp); \
        }); \
\
      MW_INFO("RpcServerModule::Setup succeeded."); \
    } catch (const std::exception& e) { \
      MW_ERROR("RpcServerModule::Setup failed: {}", e.what()); \
      return false; \
    } \
  } while (0) \

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                          ibmw::rpc::Status OnAddTwoInts( \
      ibmw::rpc::ContextRef ctx, \
      const dds_rpc_demo::AddTwoInts_Request& req, \
      dds_rpc_demo::AddTwoInts_Response& rsp); \

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
