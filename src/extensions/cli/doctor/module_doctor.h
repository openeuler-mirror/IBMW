// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "cli/command.h"

namespace ibmw::cli::doctor {

struct ModuleDoctorOptions {
  std::filesystem::path registry_path;
  std::string runtime;
};

struct ModuleCheckSummary {
  size_t ok = 0, warnings = 0, skipped = 0, query_errors = 0;
};

void AddModuleDoctorOptions(Command& command, bool root);
ModuleDoctorOptions PrepareModuleCheck(const Invocation& invocation);
ModuleCheckSummary RunModuleChecks(const ModuleDoctorOptions& options, const Invocation& invocation);
int RunModuleDoctor(const Invocation& invocation);

}  // namespace ibmw::cli::doctor
