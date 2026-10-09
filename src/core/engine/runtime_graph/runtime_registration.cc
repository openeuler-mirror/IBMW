// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/runtime_registration.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "base/exception.h"
#include "engine/runtime_graph/runtime_protocol.h"
#include "engine/runtime_graph/runtime_directory.h"

namespace ibmw::runtime::core::runtime_graph {

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
  nlohmann::json result{
      {"protocol_version", registration.protocol_version},
      {"runtime_id", registration.runtime_id},
      {"pid", registration.pid},
      {"process_start_time_ticks", registration.process_start_time_ticks},
      {"control_socket", registration.control_socket},
      {"topics", std::move(topics)}};
  if (registration.modules) result["modules"] = *registration.modules;
  return result;
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
  if (value.contains("modules")) {
    registration.modules = value.at("modules").get<std::vector<std::string>>();
    auto& modules = *registration.modules;
    std::sort(modules.begin(), modules.end());
    // Preserve ModuleBus names, including unnamed app-mode contexts.
    MW_ASSERT(std::adjacent_find(modules.begin(), modules.end()) == modules.end(),
              "Runtime registration contains duplicate module names.");
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
  const auto directory = OpenRuntimeDirectory(path.parent_path(), true);
  const auto payload = RuntimeRegistrationToJson(registration).dump(2) + '\n';
  static std::atomic<uint64_t> sequence{0};
  RuntimeFileDescriptor output;
  std::string temporary;
  for (size_t attempt = 0; attempt < 64; ++attempt) {
    temporary = registration_name + "." + std::to_string(::getpid()) + "." +
                std::to_string(sequence.fetch_add(1)) + ".tmp";
    output = RuntimeFileDescriptor(::openat(directory.Get(), temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.Get() >= 0) break;
    MW_ASSERT(errno == EEXIST, "Cannot create runtime registration temporary file.");
  }
  MW_ASSERT(output.Get() >= 0, "Cannot create a unique runtime registration temporary file.");
  try {
    size_t offset = 0;
    while (offset < payload.size()) {
      const auto written = ::write(output.Get(), payload.data() + offset, payload.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      MW_ASSERT(written > 0, "Failed to write runtime registration.");
      offset += static_cast<size_t>(written);
    }
    MW_ASSERT(::renameat(directory.Get(), temporary.c_str(), directory.Get(),
                         registration_name.c_str()) == 0,
              "Failed to publish runtime registration.");
  } catch (...) {
    ::unlinkat(directory.Get(), temporary.c_str(), 0);
    throw;
  }
}

}  // namespace ibmw::runtime::core::runtime_graph
