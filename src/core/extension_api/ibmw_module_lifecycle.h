// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ibmw::extension {

enum class ModuleLifecycleKind { kManaged, kAppContext };
enum class ModuleLifecyclePhase { kInitialize, kStart };
enum class ModuleLifecycleResult {
  kNotAttempted,
  kInProgress,
  kSucceeded,
  kFailed,
  kNotApplicable,
};

struct ModuleLifecycleFailure {
  ModuleLifecyclePhase phase = ModuleLifecyclePhase::kInitialize;
  std::string reason;

  bool operator==(const ModuleLifecycleFailure&) const = default;
};

// Observations of framework initialization/start attempts, not current business
// health. Results survive shutdown until the owning ModuleBus is destroyed.
struct ModuleLifecycleStatus {
  std::string module_name;
  std::string package_path;
  ModuleLifecycleKind kind = ModuleLifecycleKind::kManaged;
  // Initialization includes CoreAccess configuration and, for managed modules,
  // Setup(). App contexts have no Run() callback: start is kNotApplicable.
  ModuleLifecycleResult initialize = ModuleLifecycleResult::kNotAttempted;
  ModuleLifecycleResult start = ModuleLifecycleResult::kNotAttempted;
  std::optional<ModuleLifecycleFailure> failure;
  // Wall-clock timestamp of the last recorded transition, in Unix milliseconds.
  uint64_t updated_at_unix_ms = 0;

  bool operator==(const ModuleLifecycleStatus&) const = default;
};

}  // namespace ibmw::extension
