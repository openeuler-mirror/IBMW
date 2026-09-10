// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/runtime_registry.h"

#include <algorithm>
#include <cerrno>
#include <fstream>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

void RemoveOwnedSiblingSocket(
    const std::filesystem::path& registry_path,
    const std::filesystem::path& registration_path) {
  const auto socket_path =
      registry_path / (registration_path.stem().string() + ".sock");
  struct stat socket_stat {};
  if (::lstat(socket_path.c_str(), &socket_stat) != 0 ||
      !S_ISSOCK(socket_stat.st_mode) ||
      socket_stat.st_uid != ::geteuid() ||
      (socket_stat.st_mode & 077) != 0) {
    return;
  }
  std::error_code error;
  std::filesystem::remove(socket_path, error);
}

}  // namespace

std::vector<RuntimeRegistration> RuntimeRegistry::DiscoverLocalRuntimes() const {
  std::vector<RuntimeRegistration> result;
  if (!std::filesystem::exists(registry_path_)) {
    return result;
  }
  struct stat directory_stat {};
  if (::stat(registry_path_.c_str(), &directory_stat) != 0 ||
      !S_ISDIR(directory_stat.st_mode) ||
      directory_stat.st_uid != ::geteuid() ||
      (directory_stat.st_mode & 077) != 0) {
    return result;
  }

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
      std::ifstream input(entry.path());
      if (!input.is_open()) {
        continue;
      }
      if (std::filesystem::file_size(entry.path()) >=
          kRuntimeMaxControlMessage) {
        std::error_code error;
        std::filesystem::remove(entry.path(), error);
        RemoveOwnedSiblingSocket(registry_path_, entry.path());
        continue;
      }
      nlohmann::json value;
      input >> value;
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
      const bool stale = !IsProcessAlive(registration.pid) ||
          !process_start_time.has_value() ||
          *process_start_time != registration.process_start_time_ticks ||
          !registration_name_matches || !valid_socket;
      if (stale) {
        std::error_code error;
        if (socket_in_registry && socket_stat.st_uid == ::geteuid() &&
            S_ISSOCK(socket_stat.st_mode) &&
            (socket_stat.st_mode & 077) == 0) {
          std::filesystem::remove(configured_socket, error);
        }
        std::filesystem::remove(entry.path(), error);
        continue;
      }
      result.emplace_back(std::move(registration));
    } catch (...) {
      // A corrupt registration cannot be parsed to recover its advertised
      // socket path.  The registration filename is trusted only after the
      // owner-only directory and `.json` suffix checks above; derive the
      // matching socket name locally so crashes/corruption do not leave an
      // orphan endpoint behind.
      std::error_code error;
      std::filesystem::remove(entry.path(), error);
      RemoveOwnedSiblingSocket(registry_path_, entry.path());
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

}  // namespace ibmw::extensions::record_playback_extension
