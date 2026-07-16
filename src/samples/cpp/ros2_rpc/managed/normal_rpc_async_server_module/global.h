// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/logger/logger.h"

namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module {

void SetLogger(ibmw::logger::LoggerRef);
ibmw::logger::LoggerRef GetLogger();

}  // namespace ibmw::examples::cpp::ros2_rpc::normal_rpc_async_server_module
