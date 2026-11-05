// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <string>
#include <string_view>
#include "call_options.h"
#include "call_type_support.h"

namespace ibmw::cli::service {
struct CallResult {
  int exit_code = 0;
  std::string response;
};
// One request, no retries. Type libraries and message objects outlive Engine shutdown.
CallResult ExecuteCall(CallOptions options, CallTypes types, std::string_view json_request,
                       std::string_view diagnostic_name = "ibmw service call");
}  // namespace ibmw::cli::service
