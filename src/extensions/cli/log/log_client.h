// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cli/command.h"

namespace ibmw::cli::log {

struct CallFailure { int exit_code; };

// Reuse service-call connection options and --service; positional arguments are
// ignored. An empty module list queries every registered logger in that service.
// Returns validated, uppercase levels without printing the result. Transport
// diagnostics use diagnostic_name; transport failures preserve their exit code.
std::map<std::string, std::string> QueryLogLevels(
    const Invocation& connection, const std::vector<std::string>& module_names,
    std::string_view diagnostic_name = "ibmw log");

void SetLogLevel(const Invocation& connection, std::string_view module_name,
                 std::string_view level, std::string_view diagnostic_name = "ibmw log");

}  // namespace ibmw::cli::log
