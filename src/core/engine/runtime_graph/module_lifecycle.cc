// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/module_lifecycle.h"

#include <cstddef>
#include <string_view>
#include <utility>

#include "base/exception.h"

namespace ibmw::runtime::core::runtime_graph {
namespace {

using ibmw::extension::ModuleLifecycleFailure;
using ibmw::extension::ModuleLifecycleKind;
using ibmw::extension::ModuleLifecyclePhase;
using ibmw::extension::ModuleLifecycleResult;
using ibmw::extension::ModuleLifecycleStatus;

constexpr std::pair<ModuleLifecycleKind, std::string_view> kKinds[] = {
    {ModuleLifecycleKind::kManaged, "managed"},
    {ModuleLifecycleKind::kAppContext, "app_context"}};
constexpr std::pair<ModuleLifecyclePhase, std::string_view> kPhases[] = {
    {ModuleLifecyclePhase::kInitialize, "initialize"},
    {ModuleLifecyclePhase::kStart, "start"}};
constexpr std::pair<ModuleLifecycleResult, std::string_view> kResults[] = {
    {ModuleLifecycleResult::kNotAttempted, "not_attempted"},
    {ModuleLifecycleResult::kInProgress, "in_progress"},
    {ModuleLifecycleResult::kSucceeded, "succeeded"},
    {ModuleLifecycleResult::kFailed, "failed"},
    {ModuleLifecycleResult::kNotApplicable, "not_applicable"}};

template <typename Enum, std::size_t N>
std::string_view ToName(Enum value, const std::pair<Enum, std::string_view> (&names)[N]) {
  for (const auto& [candidate, name] : names) {
    if (candidate == value) return name;
  }
  MW_ASSERT(false, "Invalid module lifecycle enum value.");
  return {};
}

template <typename Enum, std::size_t N>
Enum FromName(const nlohmann::json& value,
              const std::pair<Enum, std::string_view> (&names)[N]) {
  const auto name = value.get<std::string>();
  for (const auto& [candidate, candidate_name] : names) {
    if (candidate_name == name) return candidate;
  }
  MW_ASSERT(false, "Unknown module lifecycle value '{}'.", name);
  return {};
}

}  // namespace

nlohmann::json ModuleLifecycleStatusToJson(const ModuleLifecycleStatus& status) {
  nlohmann::json value{
      {"module_name", status.module_name},
      {"package_path", status.package_path},
      {"kind", ToName(status.kind, kKinds)},
      {"initialize", ToName(status.initialize, kResults)},
      {"start", ToName(status.start, kResults)},
      {"failure", nullptr},
      {"updated_at_unix_ms", status.updated_at_unix_ms}};
  if (status.failure) {
    value["failure"] = {{"phase", ToName(status.failure->phase, kPhases)},
                        {"reason", status.failure->reason}};
  }
  return value;
}

ModuleLifecycleStatus ModuleLifecycleStatusFromJson(const nlohmann::json& value) {
  const auto& timestamp = value.at("updated_at_unix_ms");
  MW_ASSERT(timestamp.is_number_unsigned() ||
                (timestamp.is_number_integer() && timestamp.get<int64_t>() >= 0),
            "Invalid module lifecycle timestamp.");
  ModuleLifecycleStatus status{
      .module_name = value.at("module_name").get<std::string>(),
      .package_path = value.at("package_path").get<std::string>(),
      .kind = FromName(value.at("kind"), kKinds),
      .initialize = FromName(value.at("initialize"), kResults),
      .start = FromName(value.at("start"), kResults),
      .updated_at_unix_ms = timestamp.get<uint64_t>()};
  const auto& failure = value.at("failure");
  if (!failure.is_null()) {
    status.failure = ModuleLifecycleFailure{
        .phase = FromName(failure.at("phase"), kPhases),
        .reason = failure.at("reason").get<std::string>()};
  }
  return status;
}

}  // namespace ibmw::runtime::core::runtime_graph
