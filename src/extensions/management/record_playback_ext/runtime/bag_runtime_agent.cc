// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/bag_runtime_agent.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

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
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    MW_ASSERT(fd_ >= 0, "Failed to open Runtime ID lock '{}'.", path.string());
    if (::flock(fd_, LOCK_EX) != 0) {
      const int error = errno;
      ::close(fd_);
      fd_ = -1;
      MW_ASSERT(
          false, "Failed to lock Runtime ID '{}': {}.", path.string(),
          std::strerror(error));
    }
  }

  ~RuntimeIdLock() {
    if (fd_ >= 0) {
      static_cast<void>(::flock(fd_, LOCK_UN));
      ::close(fd_);
    }
  }

  RuntimeIdLock(const RuntimeIdLock&) = delete;
  RuntimeIdLock& operator=(const RuntimeIdLock&) = delete;

 private:
  int fd_ = -1;
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

BagRuntimeAgent::BagRuntimeAgent(Options options)
    : options_(std::move(options)) {}

BagRuntimeAgent::~BagRuntimeAgent() {
  Shutdown();
}

void BagRuntimeAgent::SetStartTapHandler(StartTapHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated, "Set handlers before Initialize.");
  start_tap_handler_ = std::move(handler);
}

void BagRuntimeAgent::SetStopTapHandler(StopTapHandler handler) {
  MW_ASSERT(state_.load() == State::kCreated, "Set handlers before Initialize.");
  stop_tap_handler_ = std::move(handler);
}

void BagRuntimeAgent::Initialize() {
  State expected = State::kCreated;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kInitialized),
      "Runtime Agent can only be initialized once.");

  try {
    auto& registration = options_.registration;
    MW_ASSERT(
        IsValidRuntimeId(registration.runtime_id),
        "Runtime ID must be a safe single path component (alphanumeric, '-', '_' or '.').");
    std::filesystem::create_directories(options_.registry_path);
    std::filesystem::permissions(
        options_.registry_path, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
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
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
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
    ConfigureRuntimeSocket(fd);
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

void BagRuntimeAgent::Start() {
  State expected = State::kInitialized;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kRunning),
      "Runtime Agent must be initialized before Start.");
  server_thread_ = std::thread([this] { Serve(); });
}

void BagRuntimeAgent::Shutdown() {
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
  }
}

void BagRuntimeAgent::Serve() {
  while (state_.load() == State::kRunning) {
    const int fd = server_fd_.load();
    if (fd < 0) {
      break;
    }
    const int client = ::accept(fd, nullptr, nullptr);
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
      ConfigureRuntimeSocket(client);
      HandleClient(client);
    } catch (...) {
    }
    const int active = client_fd_.exchange(-1);
    if (active >= 0) {
      ::close(active);
    }
  }
}

void BagRuntimeAgent::HandleClient(int client_fd) {
  nlohmann::json response;
  try {
    std::string request_data;
    std::array<char, 4096> buffer{};
    while (request_data.size() < kRuntimeMaxControlMessage) {
      const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
      if (received <= 0) {
        break;
      }
      request_data.append(buffer.data(), static_cast<size_t>(received));
      if (request_data.find('\n') != std::string::npos) {
        break;
      }
    }
    MW_ASSERT(
        !request_data.empty() && request_data.size() < kRuntimeMaxControlMessage,
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

nlohmann::json BagRuntimeAgent::HandleRequest(
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
  } else {
    MW_ASSERT(false, "Unknown Runtime Agent method '{}'.", method);
  }

  return nlohmann::json{{"protocol_version", kRuntimeProtocolVersion},
                        {"id", id}, {"ok", true}, {"result", result}};
}

TopicAdvertisement BagRuntimeAgent::FindTopic(
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

}  // namespace ibmw::extensions::record_playback_extension
