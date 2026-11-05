// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "ExampleRpc.ibmw_rpc.dds.h"

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                     do { \
    ctx_ptr_ = std::make_shared<ibmw::context::Context>((core_expr)); \
    ctx_ptr_->LetMe(); \
    try { \
      auto server_handle = ctx_ptr_->CreateServer<ibmw::protocols::example::ExampleServiceCoServer>(); \
    server_handle->GetFooData.ServeInline( \
        [this](ibmw::rpc::ContextRef ctx, \
               const ibmw::protocols::example::GetFooDataReq& req, \
               ibmw::protocols::example::GetFooDataRsp& rsp) { \
          return _ibmw_dds_rpc_server_marshalling_helper_rpc_server_module_get_foo_data(this, ctx, req, rsp); \
        }); \
\
    server_handle->GetBarData.ServeInline( \
        [this](ibmw::rpc::ContextRef ctx, \
               const ibmw::protocols::example::GetBarDataReq& req, \
               ibmw::protocols::example::GetBarDataRsp& rsp) { \
          return _ibmw_dds_rpc_server_marshalling_helper_rpc_server_module_get_bar_data(this, ctx, req, rsp); \
        }); \
\
      MW_INFO("RpcServerModule::Setup succeeded."); \
    } catch (const std::exception& e) { \
      MW_ERROR("RpcServerModule::Setup failed: {}", e.what()); \
      return false; \
    } \
  } while (0) \

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                          ibmw::rpc::Status OnGetFooData( \
      ibmw::rpc::ContextRef ctx, \
      const ibmw::protocols::example::GetFooDataReq& req, \
      ibmw::protocols::example::GetFooDataRsp& rsp); \
  ibmw::rpc::Status OnGetBarData( \
      ibmw::rpc::ContextRef ctx, \
      const ibmw::protocols::example::GetBarDataReq& req, \
      ibmw::protocols::example::GetBarDataRsp& rsp); \

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
