// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/runtime_registry.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/runtime_graph/runtime_protocol.h"
#include "engine/runtime_graph/runtime_directory.h"

namespace ibmw::runtime::core::runtime_graph {

std::vector<RuntimeRegistration> RuntimeRegistry::DiscoverLocalRuntimes() const {
  return DiscoverLocalRuntimesImpl(true);
}

std::vector<RuntimeRegistration> RuntimeRegistry::InspectLocalRuntimes() const {
  return DiscoverLocalRuntimesImpl(false);
}

std::vector<RuntimeRegistration> RuntimeRegistry::DiscoverLocalRuntimesImpl(
    bool cleanup_stale) const {
  std::vector<RuntimeRegistration> result;
  if (!std::filesystem::exists(registry_path_)) {
    return result;
  }
  const auto root = OpenRuntimeDirectory(registry_path_.parent_path(), false);
  const auto directory = OpenRuntimeDirectory(registry_path_, false);

  for (const auto& entry : std::filesystem::directory_iterator(registry_path_)) {
    struct stat registration_stat {};
    if (::lstat(entry.path().c_str(), &registration_stat) != 0 ||
        !S_ISREG(registration_stat.st_mode) ||
        registration_stat.st_uid != ::geteuid() ||
        (registration_stat.st_mode & 077) != 0 ||
        entry.path().extension() != ".json") {
      continue;
    }
    try {
      // Checks and reads the opened file: the entry may be replaced or grow
      // after the lstat above.
      const RuntimeFileDescriptor file(::openat(
          directory.Get(), entry.path().filename().c_str(),
          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      struct stat file_stat {};
      if (file.Get() < 0 || ::fstat(file.Get(), &file_stat) != 0 ||
          !S_ISREG(file_stat.st_mode) || file_stat.st_uid != ::geteuid() ||
          (file_stat.st_mode & 077) != 0) {
        continue;
      }
      const auto data =
          ReadRuntimeFileBelow(file.Get(), kRuntimeMaxControlMessage);
      if (!data.has_value()) {
        continue;
      }
      const auto value = nlohmann::json::parse(*data);
      auto registration = RuntimeRegistrationFromJson(value);
      const auto expected_socket_name =
          registration.runtime_id + ".sock";
      const bool registration_name_matches =
          entry.path().filename() == registration.runtime_id + ".json";
      const auto configured_socket =
          std::filesystem::path(registration.control_socket);
      const auto registry_dir = registry_path_.lexically_normal();
      const bool socket_in_registry =
          configured_socket.filename() == expected_socket_name &&
          configured_socket.parent_path().lexically_normal() == registry_dir;
      const auto process_start_time =
          ReadProcessStartTimeTicks(registration.pid);
      struct stat socket_stat {};
      const bool valid_socket =
          socket_in_registry &&
          ::lstat(configured_socket.c_str(), &socket_stat) == 0 &&
          S_ISSOCK(socket_stat.st_mode) &&
          socket_stat.st_uid == ::geteuid() &&
          (socket_stat.st_mode & 077) == 0;
      if (!registration_name_matches || !socket_in_registry) continue;
      const bool stale = !IsProcessAlive(registration.pid) ||
          (process_start_time.has_value() &&
           *process_start_time != registration.process_start_time_ticks);
      if (stale) {
        if (!cleanup_stale) continue;
        std::error_code error;
        if (socket_in_registry && socket_stat.st_uid == ::geteuid() &&
            S_ISSOCK(socket_stat.st_mode) &&
            (socket_stat.st_mode & 077) == 0) {
          std::filesystem::remove(configured_socket, error);
        }
        std::filesystem::remove(entry.path(), error);
        continue;
      }
      if (!process_start_time.has_value() || !valid_socket) continue;
      result.emplace_back(std::move(registration));
    } catch (...) {
      // Unreadable or unsupported data does not establish process death.
      continue;
    }
  }

  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.runtime_id < rhs.runtime_id;
  });
  return result;
}

std::vector<TopicAdvertisement> RuntimeRegistry::FindTopic(
    std::string_view topic_name) const {
  std::vector<TopicAdvertisement> result;
  for (const auto& runtime : DiscoverLocalRuntimes()) {
    for (const auto& topic : runtime.topics) {
      if (topic.topic.topic == topic_name) {
        result.emplace_back(topic);
      }
    }
  }
  return result;
}

bool RuntimeRegistry::IsProcessAlive(int64_t pid) noexcept {
  if (pid <= 0) {
    return false;
  }
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

}  // namespace ibmw::runtime::core::runtime_graph
