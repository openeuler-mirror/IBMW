// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "engine/runtime_graph/topic_advertisement.h"
#include "engine/runtime_graph/runtime_protocol.h"

namespace ibmw::runtime::core::runtime_graph {

using ibmw::extension::RuntimeRegistration;

std::string GenerateRuntimeId(
    std::string_view requested_id,
    std::string_view executable_name);
bool IsValidRuntimeId(std::string_view runtime_id) noexcept;
nlohmann::json RuntimeRegistrationToJson(
    const RuntimeRegistration& registration);
RuntimeRegistration RuntimeRegistrationFromJson(const nlohmann::json& value);
std::optional<uint64_t> ReadProcessStartTimeTicks(int64_t pid);
void WriteRuntimeRegistration(
    const RuntimeRegistration& registration,
    const std::filesystem::path& path);

}  // namespace ibmw::runtime::core::runtime_graph
