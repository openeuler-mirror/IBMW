// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include "cli/command.h"
#include "log_check.h"

namespace ibmw::cli::doctor {
struct LogCheckOptions {
  Invocation connection;
  std::map<std::string, std::string> expected;
  std::vector<std::string> modules;
  std::string backend, service, target, skip_reason;
};
struct LogCheckSummary {
  size_t ok = 0, warnings = 0, unknown = 0, skipped = 0;
  int interrupted = 0;
};

void AddLogDoctorOptions(Command& command, bool root);
LogCheckOptions PrepareLogCheck(const Invocation& invocation, bool root);
LogCheckSummary RunLogChecks(const LogCheckOptions& options, const Invocation& invocation);
int RunLogDoctor(const Invocation& invocation);
}  // namespace ibmw::cli::doctor
