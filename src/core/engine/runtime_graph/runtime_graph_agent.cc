// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/runtime_graph_agent.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "engine/runtime_graph/module_lifecycle.h"
#include "engine/runtime_graph/runtime_protocol.h"
#include "engine/runtime_graph/runtime_directory.h"

namespace ibmw::runtime::core::runtime_graph {

namespace {

void SendAll(int fd, std::string_view data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t written =
        ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    MW_ASSERT(written > 0, "Failed to send Runtime Agent response.");
    offset += static_cast<size_t>(written);
  }
}

sockaddr_un MakeSocketAddress(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  MW_ASSERT(
      path.size() < sizeof(address.sun_path),
      "Runtime Agent socket path is too long: '{}'.", path);
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return address;
}

class RuntimeIdLock {
 public:
  explicit RuntimeIdLock(const std::filesystem::path& path) {
    const auto directory = OpenRuntimeDirectory(path.parent_path(), false);
    fd_ = RuntimeFileDescriptor(::openat(directory.Get(), path.filename().c_str(),
        O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    MW_ASSERT(fd_.Get() >= 0, "Failed to open Runtime ID lock '{}'.", path.string());
    struct stat status {};
    MW_ASSERT(::fstat(fd_.Get(), &status) == 0 && S_ISREG(status.st_mode) &&
                  status.st_uid == ::geteuid() && (status.st_mode & 077) == 0 &&
                  status.st_nlink == 1,
              "Unsafe Runtime ID lock '{}'.", path.string());
    // Bounded: a stuck or stopped lock holder must not hang Shutdown().
    const auto deadline = std::chrono::steady_clock::now() + kRuntimeIdLockTimeout;
    while (::flock(fd_.Get(), LOCK_EX | LOCK_NB) != 0) {
      const int error = errno;
      MW_ASSERT(error == EINTR || error == EWOULDBLOCK,
                "Failed to lock Runtime ID '{}': {}.", path.string(), std::strerror(error));
      MW_ASSERT(std::chrono::steady_clock::now() < deadline,
                "Timed out locking Runtime ID '{}'.", path.string());
      if (error != EINTR) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }

  RuntimeIdLock(const RuntimeIdLock&) = delete;
  RuntimeIdLock& operator=(const RuntimeIdLock&) = delete;

 private:
  RuntimeFileDescriptor fd_;
};

std::optional<RuntimeRegistration> ReadRegistrationFile(
    const std::filesystem::path& path) {
  try {
    std::ifstream input(path);
    if (!input.is_open()) {
      return std::nullopt;
    }
    return RuntimeRegistrationFromJson(nlohmann::json::parse(input));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool IsLiveRegistration(const RuntimeRegistration& registration) {
  const auto start_time = ReadProcessStartTimeTicks(registration.pid);
  return start_time && *start_time == registration.process_start_time_ticks;
}

bool IsListeningSocket(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return false;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return true;
  }
  const auto address = MakeSocketAddress(path.string());
  const bool connected =
      ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0;
  ::close(fd);
  return connected;
}

bool SameRegistrationOwner(
    const RuntimeRegistration& lhs,
    const RuntimeRegistration& rhs) {
  return lhs.runtime_id == rhs.runtime_id && lhs.pid == rhs.pid &&
         lhs.process_start_time_ticks == rhs.process_start_time_ticks &&
         lhs.control_socket == rhs.control_socket;
}

}  // namespace

RuntimeGraphAgent::RuntimeGraphAgent(Options options)
    : options_(std::move(options)) {
  options_.registry_path = ibmw::extension::RuntimeRegistryDirectory(options_.registry_path);
}

RuntimeGraphAgent::~RuntimeGraphAgent() {
  Shutdown();
}

void RuntimeGraphAgent::UpdateRegistration(RuntimeRegistration registration) {
  const auto state = state_.load();
  MW_ASSERT(state == State::kCreated || state == State::kInitialized,
            "Runtime registration can only be updated before Start.");
  if (state == State::kInitialized) {
    MW_ASSERT(SameRegistrationOwner(registration, options_.registration),
              "Cannot change the identity of an initialized Runtime registration.");
    WriteRuntimeRegistration(registration, registration_path_);
  }
  options_.registration = std::move(registration);
}

void RuntimeGraphAgent::SetStartTapHandler(StartTapHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated || state_.load() == State::kInitialized,
            "Set handlers before Start.");
  start_tap_handler_ = std::move(handler);
}

void RuntimeGraphAgent::SetStopTapHandler(StopTapHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated || state_.load() == State::kInitialized,
            "Set handlers before Start.");
  stop_tap_handler_ = std::move(handler);
}

void RuntimeGraphAgent::SetStartPlaybackHandler(StartPlaybackHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated || state_.load() == State::kInitialized,
            "Set handlers before Start.");
  start_playback_handler_ = std::move(handler);
}

void RuntimeGraphAgent::SetStopPlaybackHandler(StopPlaybackHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated || state_.load() == State::kInitialized,
            "Set handlers before Start.");
  stop_playback_handler_ = std::move(handler);
}

void RuntimeGraphAgent::SetListModuleLifecycleHandler(ListModuleLifecycleHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated || state_.load() == State::kInitialized,
            "Set handlers before Start.");
  list_module_lifecycle_handler_ = std::move(handler);
}

void RuntimeGraphAgent::Initialize() {
  State expected = State::kCreated;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kInitialized),
      "Runtime Agent can only be initialized once.");

  try {
    auto& registration = options_.registration;
    MW_ASSERT(
        IsValidRuntimeId(registration.runtime_id),
        "Runtime ID must be a safe single path component (alphanumeric, '-', '_' or '.').");
    const auto root = OpenRuntimeDirectory(options_.registry_path.parent_path(), true);
    const auto directory = OpenRuntimeDirectory(options_.registry_path, true);
    registration.pid = static_cast<int64_t>(::getpid());
    const auto process_start_time =
        ReadProcessStartTimeTicks(registration.pid);
    MW_ASSERT(
        process_start_time.has_value(),
        "Cannot determine Runtime process start time.");
    registration.process_start_time_ticks = *process_start_time;
    registration.control_socket =
        (options_.registry_path / (registration.runtime_id + ".sock")).string();
    registration_path_ =
        options_.registry_path / (registration.runtime_id + ".json");
    for (auto& topic : registration.topics) {
      topic.runtime_id = registration.runtime_id;
      topic.control_socket = registration.control_socket;
    }

    RuntimeIdLock runtime_lock(
        options_.registry_path / (registration.runtime_id + ".lock"));
    const auto existing = ReadRegistrationFile(registration_path_);
    MW_ASSERT(
        !existing || !IsLiveRegistration(*existing),
        "Runtime ID '{}' is already used by PID {}.",
        registration.runtime_id, existing ? existing->pid : 0);
    MW_ASSERT(
        !IsListeningSocket(registration.control_socket),
        "Runtime ID '{}' already has an active control socket.",
        registration.runtime_id);
    std::filesystem::remove(registration.control_socket);
    std::filesystem::remove(registration_path_);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    MW_ASSERT(fd >= 0, "Failed to create Runtime Agent socket.");
    server_fd_ = fd;
    const auto address = MakeSocketAddress(registration.control_socket);
    MW_ASSERT(
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == 0,
        "Failed to bind Runtime Agent socket '{}': {}.",
        registration.control_socket, std::strerror(errno));
    std::filesystem::permissions(
        registration.control_socket,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    // Idle control traffic must not time out accept() and stop the service.
    // Apply request I/O deadlines only to accepted sockets in Serve().
    MW_ASSERT(::listen(fd, 16) == 0, "Failed to listen on Runtime Agent socket.");
    WriteRuntimeRegistration(registration, registration_path_);
  } catch (...) {
    const int fd = server_fd_.exchange(-1);
    if (fd >= 0) {
      ::close(fd);
      std::error_code error;
      std::filesystem::remove(options_.registration.control_socket, error);
      std::filesystem::remove(registration_path_, error);
    }
    state_ = State::kStopped;
    throw;
  }
}

void RuntimeGraphAgent::Start() {
  State expected = State::kInitialized;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kRunning),
      "Runtime Agent must be initialized before Start.");
  server_thread_ = std::thread([this] { Serve(); });
}

void RuntimeGraphAgent::Shutdown() {
  const State previous = state_.exchange(State::kStopped);
  if (previous == State::kStopped || previous == State::kCreated) {
    return;
  }

  const int fd = server_fd_.exchange(-1);
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  const int client = client_fd_.exchange(-1);
  if (client >= 0) {
    ::shutdown(client, SHUT_RDWR);
  }
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
  if (client >= 0) {
    ::close(client);
  }
  if (!registration_path_.empty()) {
    // Shutdown() runs from the destructor: a leftover registration is only
    // stale (discovery drops it once this process is gone), not an error.
    try {
      RuntimeIdLock runtime_lock(
          options_.registry_path /
          (options_.registration.runtime_id + ".lock"));
      const auto existing = ReadRegistrationFile(registration_path_);
      if (!existing || SameRegistrationOwner(*existing, options_.registration)) {
        std::error_code error;
        std::filesystem::remove(
            options_.registration.control_socket, error);
        std::filesystem::remove(registration_path_, error);
      }
    } catch (...) {
    }
  }
}

void RuntimeGraphAgent::Serve() {
  while (state_.load() == State::kRunning) {
    const int fd = server_fd_.load();
    if (fd < 0) {
      break;
    }
    const int client = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    client_fd_ = client;
    if (state_.load() != State::kRunning) {
      const int active = client_fd_.exchange(-1);
      if (active >= 0) {
        ::close(active);
      }
      break;
    }
    try {
      RequireSameUid(client);
      ConfigureRuntimeSocket(client, kRuntimeRequestTimeout);
      HandleClient(client);
    } catch (...) {
    }
    const int active = client_fd_.exchange(-1);
    if (active >= 0) {
      ::close(active);
    }
  }
}

void RuntimeGraphAgent::HandleClient(int client_fd) {
  nlohmann::json response;
  try {
    std::string request_data;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + kRuntimeRequestTimeout;
    bool complete = false;
    while (!complete && request_data.size() < kRuntimeMaxControlMessage) {
      const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      MW_ASSERT(received >= 0, "Runtime Agent request timed out.");
      MW_ASSERT(received > 0, "Runtime Agent request ended without a newline.");
      complete = std::memchr(buffer.data(), '\n', static_cast<size_t>(received)) != nullptr;
      request_data.append(buffer.data(), static_cast<size_t>(received));
      MW_ASSERT(complete || std::chrono::steady_clock::now() < deadline,
                "Runtime Agent request timed out.");
    }
    MW_ASSERT(
        complete && request_data.size() < kRuntimeMaxControlMessage,
        "Invalid Runtime Agent request size.");
    response = HandleRequest(nlohmann::json::parse(request_data));
  } catch (const std::exception& error) {
    response = nlohmann::json{
        {"protocol_version", kRuntimeProtocolVersion}, {"id", nullptr},
        {"ok", false}, {"error", error.what()}};
  }
  const std::string response_data = response.dump() + "\n";
  MW_ASSERT(
      response_data.size() < kRuntimeMaxControlMessage,
      "Runtime Agent response is too large.");
  SendAll(client_fd, response_data);
}

nlohmann::json RuntimeGraphAgent::HandleRequest(
    const nlohmann::json& request) {
  MW_ASSERT(
      request.value("protocol_version", 0u) == kRuntimeProtocolVersion,
      "Unsupported Runtime Agent request protocol version.");
  const auto id = request.value("id", nlohmann::json(nullptr));
  const std::string method = request.at("method").get<std::string>();
  const auto params = request.value("params", nlohmann::json::object());
  nlohmann::json result;

  if (method == "ListTopics") {
    result = nlohmann::json::array();
    for (const auto& topic : options_.registration.topics) {
      result.push_back(TopicAdvertisementToJson(topic));
    }
  } else if (method == "ListModuleLifecycle") {
    MW_ASSERT(list_module_lifecycle_handler_, "Runtime does not support ListModuleLifecycle.");
    result = nlohmann::json::array();
    for (const auto& status : list_module_lifecycle_handler_()) {
      result.push_back(ModuleLifecycleStatusToJson(status));
    }
  } else if (method == "GetTopicDescriptor") {
    result = TopicAdvertisementToJson(FindTopic(params));
  } else if (method == "StartTap") {
    MW_ASSERT(start_tap_handler_, "Runtime does not support StartTap.");
    const auto session = start_tap_handler_(FindTopic(params));
    MW_ASSERT(
        !session.session_id.empty() && !session.stream_socket.empty(),
        "StartTap returned an invalid session.");
    result = nlohmann::json{
        {"session_id", session.session_id},
        {"stream_socket", session.stream_socket}};
  } else if (method == "StopTap") {
    MW_ASSERT(stop_tap_handler_, "Runtime does not support StopTap.");
    const std::string session_id = params.at("session_id").get<std::string>();
    result = nlohmann::json{
        {"stopped", stop_tap_handler_(session_id)}};
  } else if (method == "StartPlayback") {
    MW_ASSERT(
        start_playback_handler_, "Runtime does not support StartPlayback.");
    const auto session = start_playback_handler_(FindTopic(params));
    MW_ASSERT(
        !session.session_id.empty() && !session.stream_socket.empty(),
        "StartPlayback returned an invalid session.");
    result = nlohmann::json{
        {"session_id", session.session_id},
        {"stream_socket", session.stream_socket}};
  } else if (method == "StopPlayback") {
    MW_ASSERT(
        stop_playback_handler_, "Runtime does not support StopPlayback.");
    const std::string session_id = params.at("session_id").get<std::string>();
    result = nlohmann::json{
        {"stopped", stop_playback_handler_(session_id)}};
  } else {
    MW_ASSERT(false, "Unknown Runtime Agent method '{}'.", method);
  }

  return nlohmann::json{{"protocol_version", kRuntimeProtocolVersion},
                        {"id", id}, {"ok", true}, {"result", result}};
}

TopicAdvertisement RuntimeGraphAgent::FindTopic(
    const nlohmann::json& params) const {
  const std::string topic_name = params.at("topic").get<std::string>();
  const std::string publisher_id = params.value("publisher_id", "");
  for (const auto& topic : options_.registration.topics) {
    if (topic.topic.topic == topic_name &&
        (publisher_id.empty() || topic.publisher_id == publisher_id)) {
      return topic;
    }
  }
  MW_ASSERT(false, "Topic '{}' is not advertised by this Runtime.", topic_name);
  return {};
}

}  // namespace ibmw::runtime::core::runtime_graph
