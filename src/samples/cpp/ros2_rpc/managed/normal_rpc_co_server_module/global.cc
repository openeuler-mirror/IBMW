// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_rpc_co_server_module/global.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module {

ibmw::logger::LoggerRef global_logger;
void SetLogger(ibmw::logger::LoggerRef logger) { global_logger = logger; }
ibmw::logger::LoggerRef GetLogger() {
  return global_logger ? global_logger : ibmw::logger::GetSimpleLoggerRef();
}

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_co_server_module
