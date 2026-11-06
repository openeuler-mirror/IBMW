// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "module_check.h"

#include <algorithm>
#include <stdexcept>

namespace ibmw::cli::doctor {
namespace {
using extension::ModuleLifecycleKind;
using extension::ModuleLifecyclePhase;
using extension::ModuleLifecycleResult;
using extension::ModuleLifecycleStatus;

void Validate(const ModuleLifecycleStatus& item) {
  const auto invalid = [&] {
    throw std::invalid_argument("Invalid lifecycle record for module '" + item.module_name + "'.");
  };
  if (item.module_name.empty()) invalid();
  if (item.kind != ModuleLifecycleKind::kManaged && item.kind != ModuleLifecycleKind::kAppContext) invalid();
  if (item.initialize == ModuleLifecycleResult::kNotApplicable) invalid();
  if (item.kind == ModuleLifecycleKind::kAppContext) {
    if (item.start != ModuleLifecycleResult::kNotApplicable) invalid();
  } else {
    if (item.start == ModuleLifecycleResult::kNotApplicable) invalid();
    if (item.initialize != ModuleLifecycleResult::kSucceeded &&
        item.start != ModuleLifecycleResult::kNotAttempted) invalid();
  }
  if (item.failure) {
    if (item.failure->phase == ModuleLifecyclePhase::kInitialize) {
      if (item.initialize != ModuleLifecycleResult::kFailed) invalid();
    } else if (item.failure->phase == ModuleLifecyclePhase::kStart) {
      if (item.start != ModuleLifecycleResult::kFailed) invalid();
    } else invalid();
  }
}

ModuleCheckResult CheckPhase(const ModuleLifecycleStatus& item, ModuleLifecyclePhase phase) {
  const bool initialize = phase == ModuleLifecyclePhase::kInitialize;
  const auto outcome = initialize ? item.initialize : item.start;
  ModuleCheckResult result{
      .module = item.module_name,
      .kind = item.kind == ModuleLifecycleKind::kManaged ? "managed" : "app_context",
      .phase = initialize ? "initialize" : "start",
      .updated_at_unix_ms = item.updated_at_unix_ms};
  switch (outcome) {
    case ModuleLifecycleResult::kSucceeded:
      result.status = CheckStatus::kOk;
      result.message = initialize ? "Recorded initialization succeeded." : "Recorded start succeeded.";
      break;
    case ModuleLifecycleResult::kFailed:
      result.status = CheckStatus::kWarning;
      result.message = initialize ? "Recorded initialization failed." : "Recorded start failed.";
      if (item.failure && item.failure->phase == phase && !item.failure->reason.empty())
        result.message += " Reason: " + item.failure->reason;
      break;
    case ModuleLifecycleResult::kNotAttempted:
      result.message = "This lifecycle phase has not been attempted.";
      break;
    case ModuleLifecycleResult::kInProgress:
      result.message = "This lifecycle phase is in progress; no completed result is recorded.";
      break;
    case ModuleLifecycleResult::kNotApplicable:
      result.message = "App contexts have no managed Run() callback.";
      break;
    default:
      throw std::invalid_argument("Invalid lifecycle phase result for module '" + item.module_name + "'.");
  }
  return result;
}
}  // namespace

std::vector<ModuleCheckResult> CheckModuleLifecycle(
    const std::vector<ModuleLifecycleStatus>& snapshot) {
  auto sorted = snapshot;
  std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
    return left.module_name < right.module_name;
  });
  std::vector<ModuleCheckResult> results;
  results.reserve(sorted.size() * 2);
  for (size_t index = 0; index < sorted.size(); ++index) {
    const auto& item = sorted[index];
    Validate(item);
    if (index && item.module_name == sorted[index - 1].module_name)
      throw std::invalid_argument("Duplicate lifecycle record for module '" + item.module_name + "'.");
    results.push_back(CheckPhase(item, ModuleLifecyclePhase::kInitialize));
    results.push_back(CheckPhase(item, ModuleLifecyclePhase::kStart));
  }
  return results;
}
}  // namespace ibmw::cli::doctor
