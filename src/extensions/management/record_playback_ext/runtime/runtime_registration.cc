// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/runtime_registration.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

bool IsValidRuntimeId(std::string_view value) noexcept {
  // Runtime IDs are used as single path components for the registration,
  // control socket and lock files.  Keep the on-disk IPC namespace local to
  // the registry directory and reject traversal/separators at the protocol
  // boundary.
  if (value.empty() || value.size() > 128 || value == "." || value == "..") {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '-' || ch == '_' || ch == '.';
  });
}

std::string GenerateRuntimeId(
    std::string_view requested_id,
    std::string_view executable_name) {
  std::string value = requested_id.empty()
                          ? std::string(executable_name)
                          : std::string(requested_id);
  if (value.empty()) {
    value = "ibmw-runtime";
  }
  std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) || ch == '-' || ch == '_' ? ch : '-';
  });
  if (requested_id.empty()) {
    value += "-" + std::to_string(static_cast<int64_t>(::getpid()));
  }
  return value;
}

nlohmann::json RuntimeRegistrationToJson(
    const RuntimeRegistration& registration) {
  nlohmann::json topics = nlohmann::json::array();
  for (const auto& topic : registration.topics) {
    topics.push_back(TopicAdvertisementToJson(topic));
  }
  return nlohmann::json{
      {"protocol_version", registration.protocol_version},
      {"runtime_id", registration.runtime_id},
      {"pid", registration.pid},
      {"process_start_time_ticks", registration.process_start_time_ticks},
      {"control_socket", registration.control_socket},
      {"topics", std::move(topics)}};
}

RuntimeRegistration RuntimeRegistrationFromJson(const nlohmann::json& value) {
  MW_ASSERT(value.is_object(), "Runtime registration must be a JSON object.");
  RuntimeRegistration registration;
  registration.protocol_version = value.value("protocol_version", 0u);
  MW_ASSERT(
      registration.protocol_version == kRuntimeProtocolVersion,
      "Unsupported Runtime registration protocol version {}.",
      registration.protocol_version);
  registration.runtime_id = value.at("runtime_id").get<std::string>();
  registration.pid = value.at("pid").get<int64_t>();
  registration.process_start_time_ticks =
      value.at("process_start_time_ticks").get<uint64_t>();
  registration.control_socket =
      value.at("control_socket").get<std::string>();
  for (const auto& topic : value.at("topics")) {
    registration.topics.emplace_back(TopicAdvertisementFromJson(topic));
  }
  MW_ASSERT(
      !registration.runtime_id.empty() && registration.pid > 0 &&
          registration.process_start_time_ticks > 0 &&
          !registration.control_socket.empty() &&
          IsValidRuntimeId(registration.runtime_id),
      "Runtime registration contains an invalid identity.");
  return registration;
}

std::optional<uint64_t> ReadProcessStartTimeTicks(int64_t pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
  if (!input.is_open()) {
    return std::nullopt;
  }
  std::string line;
  std::getline(input, line);
  const auto command_end = line.rfind(')');
  if (command_end == std::string::npos || command_end + 2 >= line.size()) {
    return std::nullopt;
  }
  std::istringstream fields(line.substr(command_end + 2));
  std::string value;
  for (int field = 3; field <= 22; ++field) {
    if (!(fields >> value)) {
      return std::nullopt;
    }
    if (field == 22) {
      try {
        size_t consumed = 0;
        const auto result = std::stoull(value, &consumed);
        if (consumed == value.size() && result > 0) {
          return result;
        }
      } catch (...) {
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void WriteRuntimeRegistration(
    const RuntimeRegistration& registration,
    const std::filesystem::path& path) {
  MW_ASSERT(
      registration.protocol_version == kRuntimeProtocolVersion,
      "Unsupported Runtime registration protocol version {}.",
      registration.protocol_version);
  MW_ASSERT(
      IsValidRuntimeId(registration.runtime_id),
      "Runtime registration contains an invalid runtime ID.");
  const auto registry_dir = path.parent_path().lexically_normal();
  const auto registration_name = path.filename().string();
  const auto expected_registration_name = registration.runtime_id + ".json";
  const auto socket_path =
      std::filesystem::path(registration.control_socket);
  MW_ASSERT(
      registration_name == expected_registration_name &&
          socket_path.filename() == registration.runtime_id + ".sock" &&
          socket_path.parent_path().lexically_normal() == registry_dir,
      "Runtime registration paths must stay within the registry directory.");
  std::filesystem::create_directories(path.parent_path());
  std::filesystem::permissions(
      path.parent_path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    MW_ASSERT(
        output.is_open(), "Cannot write Runtime registration '{}'.",
        temporary);
    output << RuntimeRegistrationToJson(registration).dump(2) << '\n';
    output.close();
    MW_ASSERT(output.good(), "Failed to write Runtime registration.");
  }
  std::filesystem::permissions(
      temporary,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  std::filesystem::rename(temporary, path);
}

}  // namespace ibmw::extensions::record_playback_extension
