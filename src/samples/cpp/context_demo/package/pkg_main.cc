// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <functional>
#include <string_view>
#include <tuple>

#include "pkg/pkg_macro.h"
#include "chn_publisher_module/transport_publisher_module.h"
#include "chn_subscriber_inline_module/chn_subscriber_inline_module.h"
#include "chn_subscriber_on_executor_module/chn_subscriber_on_executor_module.h"
#include "executor/executor_module.h"
#include "logger/logger_module.h"
#include "rpc_client_module/rpc_client_module.h"
#include "rpc_server_inline_module/rpc_server_inline_module.h"
#include "rpc_server_on_executor_module/rpc_server_on_executor_module.h"

using ibmw::examples::cpp::context::transport_publisher_module::ChnPublisherModule;
using ibmw::examples::cpp::context::ChnSubscriberInlineModule::ChnSubscriberInlineModule;
using ibmw::examples::cpp::context::ChnSubscriberOnExecutorModule::ChnSubscriberOnExecutorModule;
using ibmw::examples::cpp::context::executor_module::ExecutorModule;
using ibmw::examples::cpp::context::logger_module::LoggerModule;
using ibmw::examples::cpp::context::RpcClientModule::RpcClientModule;
using ibmw::examples::cpp::context::RpcServerInlineModule::RpcServerInlineModule;
using ibmw::examples::cpp::context::RpcServerOnExecutorModule::RpcServerOnExecutorModule;

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"ContextExecutorModule", []() -> ibmw::Module* { return new ExecutorModule(); }},
    {"ContextTransportPublisherModule", []() -> ibmw::Module* { return new ChnPublisherModule(); }},
    {"ContextSubscriberOnExecutorModule", []() -> ibmw::Module* { return new ChnSubscriberOnExecutorModule(); }},
    {"ContextSubscriberInlineModule", []() -> ibmw::Module* { return new ChnSubscriberInlineModule(); }},
    {"ContextLoggerModule", []() -> ibmw::Module* { return new LoggerModule(); }},
    {"RpcClientModule", []() -> ibmw::Module* { return new RpcClientModule(); }},
    {"RpcServerInlineModule", []() -> ibmw::Module* { return new RpcServerInlineModule(); }},
    {"RpcServerOnExecutorModule", []() -> ibmw::Module* { return new RpcServerOnExecutorModule(); }},
};

IBMW_PKG_MAIN(ibmw_module_register_array)
