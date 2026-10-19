// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/runtime_playback_stream.h"
#include "engine/runtime_graph/runtime_directory.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

using PlaybackWireHeader = RuntimeStreamHeader;
using PlaybackTerminalHeader = RuntimeTerminalHeader;

enum class ReceiveStatus {
  kOk,
  kClosed,
  kTruncated,
  kError,
};

uint64_t HostToNetwork64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
         htonl(static_cast<uint32_t>(value >> 32));
#else
  return value;
#endif
}

uint64_t NetworkToHost64(uint64_t value) {
  return HostToNetwork64(value);
}

sockaddr_un MakeSocketAddress(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  MW_ASSERT(
      path.size() < sizeof(address.sun_path),
      "Runtime playback socket path is too long: '{}'.", path);
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return address;
}

bool SendAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t written =
        ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
}

ReceiveStatus ReceiveAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0) {
      return ReceiveStatus::kError;
    }
    if (received == 0) {
      return offset == 0 ? ReceiveStatus::kClosed
                         : ReceiveStatus::kTruncated;
    }
    offset += static_cast<size_t>(received);
  }
  return ReceiveStatus::kOk;
}

void RequireReceive(
    ReceiveStatus status,
    std::string_view description) {
  MW_ASSERT(
      status == ReceiveStatus::kOk,
      "Runtime playback {} receive failed: {}.", description,
      status == ReceiveStatus::kError
          ? std::strerror(errno)
          : (status == ReceiveStatus::kTruncated
                 ? "stream ended with a partial frame"
                 : "stream closed unexpectedly"));
}

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    std::vector<uint8_t> bytes) {
  auto storage = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view,
      [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

bool SendMessage(
    int fd,
    const ibmw::transport::SerializedMessage& message) {
  MW_ASSERT(message.payload != nullptr, "Playback payload cannot be null.");
  const nlohmann::json metadata{
      {"source_id", message.identity.source_id},
      {"publisher_id", message.identity.publisher_id},
      {"sequence", message.identity.sequence},
      {"publish_timestamp", message.publish_timestamp},
      {"receive_timestamp", message.receive_timestamp},
      {"source_driver", message.source_driver}};
  const std::string header_json = metadata.dump();
  const std::string payload = message.payload->JoinToString();
  MW_ASSERT(
      header_json.size() <= kRuntimeMaxStreamHeader,
      "Runtime playback header is too large.");
  MW_ASSERT(
      payload.size() <= kRuntimeMaxStreamPayload,
      "Runtime playback payload is too large.");
  PlaybackWireHeader header{
      .magic = htonl(kRuntimePlaybackMagic),
      .protocol_version = htonl(kRuntimeProtocolVersion),
      .json_size = htonl(static_cast<uint32_t>(header_json.size())),
      .reserved = 0,
      .payload_size = HostToNetwork64(payload.size())};
  return SendAll(fd, &header, sizeof(header)) &&
         SendAll(fd, header_json.data(), header_json.size()) &&
         SendAll(fd, payload.data(), payload.size());
}

std::optional<ibmw::transport::SerializedMessage>
ReceiveMessage(
    int fd,
    const TopicAdvertisement& advertisement,
    bool& clean_end) {
  clean_end = false;
  PlaybackWireHeader wire{};
  const auto header_status = ReceiveAll(fd, &wire, sizeof(wire));
  if (header_status == ReceiveStatus::kClosed) {
    clean_end = true;
    return std::nullopt;
  }
  RequireReceive(header_status, "frame header");
  const uint32_t magic = ntohl(wire.magic);
  const uint32_t protocol_version = ntohl(wire.protocol_version);
  const uint32_t json_size = ntohl(wire.json_size);
  const uint64_t payload_size = NetworkToHost64(wire.payload_size);
  MW_ASSERT(
      magic == kRuntimePlaybackMagic,
      "Invalid Runtime playback stream magic.");
  MW_ASSERT(
      protocol_version == kRuntimeProtocolVersion,
      "Unsupported Runtime playback stream protocol version {}.",
      protocol_version);
  MW_ASSERT(
      wire.reserved == 0,
      "Runtime playback stream reserved field is nonzero.");
  MW_ASSERT(
      json_size <= kRuntimeMaxStreamHeader,
      "Runtime playback header is too large.");
  MW_ASSERT(
      payload_size <= kRuntimeMaxStreamPayload,
      "Runtime playback payload is too large.");

  std::string json_data(json_size, '\0');
  std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
  RequireReceive(
      ReceiveAll(fd, json_data.data(), json_data.size()),
      "metadata");
  RequireReceive(
      ReceiveAll(fd, payload.data(), payload.size()),
      "payload");
  const auto metadata = nlohmann::json::parse(json_data);
  return ibmw::transport::SerializedMessage{
      .topic = advertisement.topic,
      .identity =
          {.source_id = metadata.at("source_id").get<std::string>(),
           .publisher_id = metadata.at("publisher_id").get<std::string>(),
           .sequence = metadata.at("sequence").get<uint64_t>()},
      .publish_timestamp =
          metadata.at("publish_timestamp").get<uint64_t>(),
      .receive_timestamp =
          metadata.at("receive_timestamp").get<uint64_t>(),
      .source_driver = metadata.at("source_driver").get<std::string>(),
      .payload = MakeOwnedPayload(std::move(payload))};
}

bool SendTerminal(
    int fd,
    bool ok,
    uint64_t message_count,
    std::string_view error) {
  const std::string json_data = nlohmann::json{
      {"ok", ok},
      {"message_count", message_count},
      {"error", error}}
                                    .dump();
  MW_ASSERT(
      json_data.size() <= kRuntimeMaxStreamHeader,
      "Runtime playback terminal frame is too large.");
  PlaybackTerminalHeader header{
      .magic = htonl(kRuntimePlaybackTerminalMagic),
      .protocol_version = htonl(kRuntimeProtocolVersion),
      .json_size = htonl(static_cast<uint32_t>(json_data.size())),
      .reserved = 0};
  return SendAll(fd, &header, sizeof(header)) &&
         SendAll(fd, json_data.data(), json_data.size());
}

void ReceiveTerminal(int fd) {
  PlaybackTerminalHeader wire{};
  RequireReceive(ReceiveAll(fd, &wire, sizeof(wire)), "terminal header");
  const uint32_t magic = ntohl(wire.magic);
  const uint32_t protocol_version = ntohl(wire.protocol_version);
  const uint32_t json_size = ntohl(wire.json_size);
  MW_ASSERT(
      magic == kRuntimePlaybackTerminalMagic,
      "Invalid Runtime playback terminal magic.");
  MW_ASSERT(
      protocol_version == kRuntimeProtocolVersion,
      "Unsupported Runtime playback terminal protocol version {}.",
      protocol_version);
  MW_ASSERT(
      wire.reserved == 0,
      "Runtime playback terminal reserved field is nonzero.");
  MW_ASSERT(
      json_size <= kRuntimeMaxStreamHeader,
      "Runtime playback terminal frame is too large.");
  std::string json_data(json_size, '\0');
  RequireReceive(
      ReceiveAll(fd, json_data.data(), json_data.size()),
      "terminal body");
  const auto terminal = nlohmann::json::parse(json_data);
  MW_ASSERT(
      terminal.at("ok").get<bool>(),
      "Runtime playback failed after {} message(s): {}.",
      terminal.value("message_count", 0ull),
      terminal.value("error", "unspecified Runtime playback failure"));
}

}  // namespace

struct RuntimePlaybackServer::Session {
  std::filesystem::path socket_path;
  TopicAdvertisement advertisement;
  MessageHandler message_handler;
  std::atomic<bool> stopping{false};
  std::atomic<int> server_fd{-1};
  std::atomic<int> client_fd{-1};
  std::thread stream_thread;

  void Start() {
    std::filesystem::remove(socket_path);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MW_ASSERT(fd >= 0, "Failed to create Runtime playback socket.");
    server_fd = fd;
    const auto address = MakeSocketAddress(socket_path.string());
    MW_ASSERT(
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == 0,
        "Failed to bind Runtime playback socket '{}': {}.",
        socket_path.string(), std::strerror(errno));
    std::filesystem::permissions(
        socket_path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    MW_ASSERT(
        ::listen(fd, 1) == 0,
        "Failed to listen on Runtime playback socket.");
    stream_thread = std::thread([this] {
      const int accepted = ::accept(server_fd.load(), nullptr, nullptr);
      if (accepted < 0) {
        return;
      }
      client_fd = accepted;
      try {
        RequireSameUid(accepted);
        ConfigureRuntimeSocket(accepted);
      } catch (...) {
        ::close(accepted);
        client_fd = -1;
        return;
      }
      bool ok = true;
      uint64_t message_count = 0;
      std::string terminal_error;
      try {
        while (!stopping.load()) {
          bool clean_end = false;
          auto message = ReceiveMessage(
              accepted, advertisement, clean_end);
          if (!message) {
            MW_ASSERT(
                clean_end || stopping.load(),
                "Runtime playback stream ended without a complete frame.");
            break;
          }
          message_handler(advertisement, std::move(*message));
          ++message_count;
        }
      } catch (const std::exception& error) {
        ok = false;
        terminal_error = error.what();
      } catch (...) {
        ok = false;
        terminal_error = "unknown Runtime playback error";
      }
      static_cast<void>(SendTerminal(
          accepted, ok, message_count, terminal_error));
      ::shutdown(accepted, SHUT_RDWR);
      ::close(accepted);
      client_fd = -1;
    });
  }

  void Stop() {
    if (stopping.exchange(true)) {
      return;
    }
    const int listen_fd = server_fd.exchange(-1);
    if (listen_fd >= 0) {
      ::shutdown(listen_fd, SHUT_RDWR);
      ::close(listen_fd);
    }
    const int connected_fd = client_fd.load();
    if (connected_fd >= 0) {
      ::shutdown(connected_fd, SHUT_RDWR);
    }
    if (stream_thread.joinable()) {
      stream_thread.join();
    }
    std::filesystem::remove(socket_path);
  }

  ~Session() { Stop(); }
};

RuntimePlaybackServer::RuntimePlaybackServer(
    std::string runtime_id,
    std::filesystem::path socket_directory,
    MessageHandler message_handler)
    : runtime_id_(std::move(runtime_id)),
      socket_directory_(ibmw::extension::RuntimeRegistryDirectory(socket_directory)),
      message_handler_(std::move(message_handler)) {
  MW_ASSERT(message_handler_, "Runtime playback handler cannot be empty.");
}

RuntimePlaybackServer::~RuntimePlaybackServer() {
  Shutdown();
}

void RuntimePlaybackServer::Start() {
  bool expected = false;
  MW_ASSERT(
      started_.compare_exchange_strong(expected, true),
      "Runtime playback server is already started.");
  const auto root = ibmw::runtime::core::runtime_graph::OpenRuntimeDirectory(
      socket_directory_.parent_path(), true);
  const auto directory = ibmw::runtime::core::runtime_graph::OpenRuntimeDirectory(
      socket_directory_, true);
}

TapSessionDescriptor RuntimePlaybackServer::StartSession(
    const TopicAdvertisement& advertisement) {
  MW_ASSERT(started_.load(), "Runtime playback server is not started.");
  MW_ASSERT(
      advertisement.runtime_id == runtime_id_,
      "Playback request runtime '{}' does not match '{}'.",
      advertisement.runtime_id, runtime_id_);
  uint64_t numeric_id = 0;
  {
    std::lock_guard lock(sessions_mutex_);
    numeric_id = next_session_id_++;
  }
  const std::string id =
      runtime_id_ + "-play-" + std::to_string(numeric_id);
  auto session = std::make_shared<Session>();
  session->socket_path =
      socket_directory_ /
      ("play-" + std::to_string(::getpid()) + "-" +
       std::to_string(numeric_id) + ".sock");
  session->advertisement = advertisement;
  session->message_handler = message_handler_;
  session->Start();
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.emplace(id, session);
  }
  return {
      .session_id = id,
      .stream_socket = session->socket_path.string()};
}

bool RuntimePlaybackServer::StopSession(std::string_view session_id) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto found = sessions_.find(std::string(session_id));
    if (found == sessions_.end()) {
      return false;
    }
    session = std::move(found->second);
    sessions_.erase(found);
  }
  session->Stop();
  return true;
}

void RuntimePlaybackServer::Shutdown() {
  if (!started_.exchange(false)) {
    return;
  }
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions.swap(sessions_);
  }
  for (auto& [id, session] : sessions) {
    static_cast<void>(id);
    session->Stop();
  }
}

UnixRuntimePlaybackClient::UnixRuntimePlaybackClient(
    std::string control_socket)
    : agent_client_(std::move(control_socket)) {}

UnixRuntimePlaybackClient::~UnixRuntimePlaybackClient() {
  try {
    Stop();
  } catch (...) {
  }
}

void UnixRuntimePlaybackClient::Start(
    const TopicAdvertisement& advertisement) {
  MW_ASSERT(stream_fd_.load() < 0, "Runtime playback client is already started.");
  const auto session = agent_client_.StartPlayback(
      advertisement.topic.topic, advertisement.publisher_id);
  session_id_ = session.session_id;
  try {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MW_ASSERT(fd >= 0, "Failed to create Runtime playback client socket.");
    stream_fd_ = fd;
    const auto address = MakeSocketAddress(session.stream_socket);
    MW_ASSERT(
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == 0,
        "Failed to connect Runtime playback stream '{}': {}.",
        session.stream_socket, std::strerror(errno));
    RequireSameUid(fd);
    ConfigureRuntimeSocket(fd);
  } catch (...) {
    const int fd = stream_fd_.exchange(-1);
    if (fd >= 0) {
      ::close(fd);
    }
    try {
      static_cast<void>(agent_client_.StopPlayback(session_id_));
    } catch (...) {
    }
    session_id_.clear();
    throw;
  }
}

void UnixRuntimePlaybackClient::Publish(
    const ibmw::transport::SerializedMessage& message) {
  const int fd = stream_fd_.load();
  MW_ASSERT(fd >= 0, "Runtime playback client is not started.");
  MW_ASSERT(SendMessage(fd, message), "Runtime playback stream was closed.");
}

void UnixRuntimePlaybackClient::Stop() {
  const int fd = stream_fd_.exchange(-1);
  if (fd < 0) {
    return;
  }
  std::exception_ptr error;
  ::shutdown(fd, SHUT_WR);
  try {
    ReceiveTerminal(fd);
  } catch (...) {
    error = std::current_exception();
  }
  ::close(fd);
  if (!session_id_.empty()) {
    try {
      static_cast<void>(agent_client_.StopPlayback(session_id_));
    } catch (...) {
      if (!error) {
        error = std::current_exception();
      }
    }
    session_id_.clear();
  }
  if (error) {
    std::rethrow_exception(error);
  }
}

}  // namespace ibmw::extensions::record_playback_extension
