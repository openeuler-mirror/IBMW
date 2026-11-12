// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "extension_api/ibmw_module_lifecycle.h"
#include "topic_check.h"

namespace ibmw::cli::doctor {

struct ModuleCheckResult {
  std::string module, kind, phase;
  CheckStatus status = CheckStatus::kSkipped;
  std::string message;
  uint64_t updated_at_unix_ms = 0;
};

// Only evaluate recorded initialization/start outcomes. Malformed snapshots
// throw a query-data error; they are not module faults or UNKNOWN check items.
std::vector<ModuleCheckResult> CheckModuleLifecycle(
    const std::vector<extension::ModuleLifecycleStatus>& snapshot);

}  // namespace ibmw::cli::doctor
