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
#include "record_playback_ext/discovery/topic_advertisement.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

struct RuntimeRegistration {
  uint32_t protocol_version = kRuntimeProtocolVersion;
  std::string runtime_id;
  int64_t pid = 0;
  uint64_t process_start_time_ticks = 0;
  std::string control_socket;
  std::vector<TopicAdvertisement> topics;
};

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

}  // namespace ibmw::extensions::record_playback_extension
