// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/runtime_tap_stream.h"
#include "engine/runtime_graph/runtime_directory.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "base/time_util.h"
#include "record_playback_ext/global.h"
#include "record_playback_ext/recorder/record_payload.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr size_t kMaxQueuedMessages = 1024;

using TapWireHeader = RuntimeStreamHeader;

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
      "Runtime tap socket path is too long: '{}'.", path);
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

enum class ReceiveStatus {
  kOk,
  kEof,
  kTruncated,
  kError,
};

struct ReceiveResult {
  ReceiveStatus status = ReceiveStatus::kOk;
  int error_number = 0;
};

ReceiveResult ReceiveAll(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
    if (received > 0) {
      offset += static_cast<size_t>(received);
      continue;
    }
    if (received == 0) {
      return {
          .status = offset == 0 ? ReceiveStatus::kEof
                                : ReceiveStatus::kTruncated};
    }
    if (errno == EINTR) {
      continue;
    }
    return {.status = ReceiveStatus::kError, .error_number = errno};
  }
  return {};
}

uint64_t ParseUint64(std::string_view value) {
  uint64_t result = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc() ? result : 0;
}

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    std::vector<uint8_t> bytes) {
  auto storage = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view, [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

bool SendFrame(
    int fd,
    const nlohmann::json& metadata,
    std::string_view payload) {
  const std::string header_json = metadata.dump();
  MW_ASSERT(
      header_json.size() <= kRuntimeMaxStreamHeader,
      "Runtime tap header is too large.");
  MW_ASSERT(
      payload.size() <= kRuntimeMaxStreamPayload,
      "Runtime tap payload is too large.");
  TapWireHeader header{
      .magic = htonl(kRuntimeTapMagic),
      .protocol_version = htonl(kRuntimeProtocolVersion),
      .json_size = htonl(static_cast<uint32_t>(header_json.size())),
      .reserved = 0,
      .payload_size = HostToNetwork64(payload.size())};
  return SendAll(fd, &header, sizeof(header)) &&
         SendAll(fd, header_json.data(), header_json.size()) &&
         SendAll(fd, payload.data(), payload.size());
}

bool SendMessage(
    int fd,
    const ibmw::transport::SerializedMessage& message,
    uint64_t dropped_total) {
  const nlohmann::json metadata{
      {"kind", "message"},
      {"source_id", message.identity.source_id},
      {"publisher_id", message.identity.publisher_id},
      {"sequence", message.identity.sequence},
      {"publish_timestamp", message.publish_timestamp},
      {"receive_timestamp", message.receive_timestamp},
      {"source_driver", message.source_driver},
      {"dropped_total", dropped_total}};
  return SendFrame(fd, metadata, message.payload->JoinToString());
}

bool SendDroppedMessages(int fd, uint64_t dropped_total) {
  return SendFrame(
      fd,
      nlohmann::json{
          {"kind", "dropped"}, {"dropped_total", dropped_total}},
      {});
}

bool SendTerminal(
    int fd,
    uint64_t dropped_total,
    std::string_view error) {
  return SendFrame(
      fd,
      nlohmann::json{
          {"kind", "terminal"},
          {"ok", error.empty()},
          {"error", error},
          {"dropped_total", dropped_total}},
      {});
}

}  // namespace

struct RuntimeTapServer::Session {
  std::string id;
  std::filesystem::path socket_path;
  TopicAdvertisement advertisement;
  std::atomic<bool> stopping{false};
  std::atomic<bool> accepting{true};
  std::atomic<int> server_fd{-1};
  std::atomic<int> client_fd{-1};
  std::mutex mutex;
  std::mutex stop_mutex;
  std::condition_variable cv;
  std::deque<ibmw::transport::SerializedMessage> queue;
  uint64_t dropped_total = 0;
  uint64_t reported_dropped_total = 0;
  std::string stream_error;
  std::thread stream_thread;

  void Fail(std::string error) {
    {
      std::lock_guard lock(mutex);
      if (stream_error.empty()) {
        stream_error = std::move(error);
      }
      accepting = false;
    }
    cv.notify_all();
  }

  void Start() {
    std::filesystem::remove(socket_path);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MW_ASSERT(fd >= 0, "Failed to create Runtime tap socket.");
    server_fd = fd;
    const auto address = MakeSocketAddress(socket_path.string());
    MW_ASSERT(
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == 0,
        "Failed to bind Runtime tap socket '{}': {}.", socket_path.string(),
        std::strerror(errno));
    std::filesystem::permissions(
        socket_path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    MW_ASSERT(::listen(fd, 1) == 0, "Failed to listen on Runtime tap socket.");

    stream_thread = std::thread([this] {
      int accepted = -1;
      try {
        accepted = ::accept(server_fd.load(), nullptr, nullptr);
        if (accepted < 0) {
          if (!stopping.load()) {
            Fail(
                "Runtime tap '" + id + "' failed to accept a client: " +
                std::strerror(errno));
          }
          return;
        }
        client_fd = accepted;
        RequireSameUid(accepted);
        ConfigureRuntimeSocket(accepted);
        if (stopping.load()) {
          ::shutdown(accepted, SHUT_RDWR);
        }
        while (true) {
          std::optional<ibmw::transport::SerializedMessage>
              message;
          uint64_t dropped_to_report = 0;
          uint64_t final_dropped_total = 0;
          std::string terminal_error;
          bool finish = false;
          {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] {
              return stopping.load() || !stream_error.empty() ||
                     !queue.empty() ||
                     dropped_total != reported_dropped_total;
            });
            if (dropped_total != reported_dropped_total) {
              reported_dropped_total = dropped_total;
              dropped_to_report = reported_dropped_total;
            }
            if (!queue.empty()) {
              message.emplace(std::move(queue.front()));
              queue.pop_front();
            } else if (
                (stopping.load() || !stream_error.empty()) &&
                dropped_total == reported_dropped_total) {
              finish = true;
              final_dropped_total = dropped_total;
              terminal_error = stream_error;
            }
          }
          if (dropped_to_report != 0 &&
              !SendDroppedMessages(accepted, dropped_to_report)) {
            if (!stopping.load()) {
              Fail(
                  "Runtime tap '" + id + "' could not report " +
                  std::to_string(dropped_to_report) +
                  " dropped message(s).");
            }
            break;
          }
          if (message &&
              !SendMessage(accepted, *message, reported_dropped_total)) {
            if (!stopping.load()) {
              Fail("Runtime tap '" + id + "' stream send failed.");
            }
            break;
          }
          if (finish) {
            if (!SendTerminal(
                    accepted, final_dropped_total, terminal_error)) {
              if (!stopping.load()) {
                Fail(
                    "Runtime tap '" + id +
                    "' failed to send its terminal status.");
              }
            }
            break;
          }
        }
      } catch (const std::exception& error) {
        Fail(
            "Runtime tap '" + id + "' stream failed: " + error.what());
        if (accepted >= 0) {
          std::string failure;
          uint64_t final_dropped_total = 0;
          {
            std::lock_guard lock(mutex);
            failure = stream_error;
            final_dropped_total = dropped_total;
          }
          static_cast<void>(
              SendTerminal(accepted, final_dropped_total, failure));
        }
      } catch (...) {
        Fail("Runtime tap '" + id + "' stream failed unexpectedly.");
        if (accepted >= 0) {
          std::string failure;
          uint64_t final_dropped_total = 0;
          {
            std::lock_guard lock(mutex);
            failure = stream_error;
            final_dropped_total = dropped_total;
          }
          static_cast<void>(
              SendTerminal(accepted, final_dropped_total, failure));
        }
      }
      accepting = false;
      if (accepted >= 0) {
        ::shutdown(accepted, SHUT_RDWR);
        ::close(accepted);
      }
      client_fd = -1;
    });
  }

  void Enqueue(ibmw::transport::SerializedMessage message) {
    std::lock_guard lock(mutex);
    if (!accepting.load()) {
      return;
    }
    if (queue.size() >= kMaxQueuedMessages) {
      ++dropped_total;
      cv.notify_one();
      return;
    }
    queue.emplace_back(std::move(message));
    cv.notify_one();
  }

  std::string Stop() {
    std::lock_guard stop_lock(stop_mutex);
    accepting = false;
    stopping = true;
    cv.notify_all();
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
    std::lock_guard lock(mutex);
    return stream_error;
  }

  ~Session() { static_cast<void>(Stop()); }
};

RuntimeTapServer::RuntimeTapServer(
    PublishTapCapability& publish_tap,
    std::string runtime_id,
    std::filesystem::path socket_directory)
    : publish_tap_(publish_tap),
      runtime_id_(std::move(runtime_id)),
      socket_directory_(ibmw::extension::RuntimeRegistryDirectory(socket_directory)) {}

RuntimeTapServer::~RuntimeTapServer() {
  Shutdown();
}

void RuntimeTapServer::Start() {
  bool expected = false;
  MW_ASSERT(started_.compare_exchange_strong(expected, true),
            "Runtime tap server is already started.");
  const auto root = ibmw::runtime::core::runtime_graph::OpenRuntimeDirectory(socket_directory_.parent_path(), true);
  const auto directory = ibmw::runtime::core::runtime_graph::OpenRuntimeDirectory(socket_directory_, true);
}

TapSessionDescriptor RuntimeTapServer::StartSession(
    const TopicAdvertisement& advertisement) {
  MW_ASSERT(started_.load(), "Runtime tap server is not started.");
  MW_ASSERT(
      advertisement.runtime_id == runtime_id_,
      "Tap request runtime '{}' does not match '{}'.",
      advertisement.runtime_id, runtime_id_);
  uint64_t numeric_id = 0;
  {
    std::lock_guard lock(sessions_mutex_);
    numeric_id = next_session_id_++;
  }
  const std::string id =
      runtime_id_ + "-tap-" + std::to_string(numeric_id);
  auto session = std::make_shared<Session>();
  session->id = id;
  session->socket_path =
      socket_directory_ /
      ("tap-" + std::to_string(::getpid()) + "-" +
       std::to_string(numeric_id) + ".sock");
  session->advertisement = advertisement;
  session->Start();
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.emplace(id, session);
    if (publish_tap_handle_ == 0) {
      publish_tap_handle_ = publish_tap_.Register(
          [this](ibmw::extension::MessageView& frame) { OnPublish(frame); });
      MW_ASSERT(publish_tap_handle_ != 0, "Failed to register publish tap.");
    }
  }
  return {.session_id = id,
          .stream_socket = session->socket_path.string()};
}

bool RuntimeTapServer::StopSession(std::string_view session_id) {
  std::shared_ptr<Session> session;
  uint64_t tap_handle = 0;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto found = sessions_.find(std::string(session_id));
    if (found == sessions_.end()) {
      return false;
    }
    session = std::move(found->second);
    sessions_.erase(found);
    if (sessions_.empty()) {
      tap_handle = publish_tap_handle_;
      publish_tap_handle_ = 0;
    }
  }
  if (tap_handle != 0) {
    publish_tap_.Unregister(tap_handle);
  }
  const std::string stream_error = session->Stop();
  MW_ASSERT(
      stream_error.empty(), "Runtime tap session '{}' failed: {}.",
      session_id, stream_error);
  return true;
}

void RuntimeTapServer::Shutdown() {
  if (!started_.exchange(false)) {
    return;
  }
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
  uint64_t tap_handle = 0;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions.swap(sessions_);
    tap_handle = publish_tap_handle_;
    publish_tap_handle_ = 0;
  }
  if (tap_handle != 0) {
    publish_tap_.Unregister(tap_handle);
  }
  for (auto& [id, session] : sessions) {
    const std::string stream_error = session->Stop();
    if (!stream_error.empty()) {
      MW_ERROR(
          "Runtime tap session '{}' stopped with an error: {}", id,
          stream_error);
    }
  }
}

void RuntimeTapServer::OnPublish(ibmw::extension::MessageView& frame) {
  const std::string publisher_id =
      std::string(frame.module_name) + "-" +
      std::to_string(frame.endpoint_index);
  std::vector<std::shared_ptr<Session>> matching;
  {
    std::lock_guard lock(sessions_mutex_);
    for (const auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session->advertisement.topic.topic == frame.topic &&
          session->advertisement.topic.msg_type == frame.msg_type &&
          session->advertisement.publisher_id == publisher_id) {
        matching.emplace_back(session);
      }
    }
  }
  if (matching.empty()) {
    return;
  }

  for (const auto& session : matching) {
    try {
      auto payload = AcquireRecordPayload(
          frame, session->advertisement.topic.serialization_type);
      MW_ASSERT(
          payload != nullptr,
          "Runtime tap serialization returned an empty payload.");
      const auto sequence = ParseUint64(
          frame.context.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ));
      const auto publish_timestamp = ParseUint64(
          frame.context.GetMetaValue(
              IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP));
      session->Enqueue(ibmw::transport::SerializedMessage{
          .topic = session->advertisement.topic,
          .identity =
              {.source_id = runtime_id_,
               .publisher_id = publisher_id,
               .sequence = sequence},
          .publish_timestamp = publish_timestamp,
          .receive_timestamp = ibmw::common::util::GetCurTimestampNs(),
          .source_driver = "runtime-tap",
          .payload = ibmw::transport::CopySerializedPayload(
              *payload)});
    } catch (const std::exception& error) {
      session->Fail(
          "Runtime tap '" + session->id + "' failed to capture topic '" +
              std::string(frame.topic) + "': " + error.what());
    } catch (...) {
      session->Fail(
          "Runtime tap '" + session->id + "' failed to capture topic '" +
              std::string(frame.topic) + "'.");
    }
  }
}

UnixRuntimeTapClient::UnixRuntimeTapClient(
    std::string control_socket,
    DroppedMessageCallback dropped_message_callback,
    StreamErrorCallback stream_error_callback)
    : control_socket_(control_socket),
      agent_client_(std::move(control_socket)),
      dropped_message_callback_(std::move(dropped_message_callback)),
      stream_error_callback_(std::move(stream_error_callback)) {}

UnixRuntimeTapClient::~UnixRuntimeTapClient() {
  try {
    StopTap();
  } catch (...) {
  }
}

void UnixRuntimeTapClient::StartTap(
    const TopicAdvertisement& advertisement,
    ibmw::transport::SerializedMessageCallback callback) {
  bool expected = false;
  MW_ASSERT(
      running_.compare_exchange_strong(expected, true),
      "Runtime tap client is already running.");
  try {
    reported_dropped_total_ = 0;
    stop_requested_ = false;
    terminal_received_ = false;
    remote_closed_ = false;
    {
      std::lock_guard lock(stream_error_mutex_);
      stream_error_.reset();
    }
    const auto session = agent_client_.StartTap(
        advertisement.topic.topic, advertisement.publisher_id);
    session_id_ = session.session_id;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    MW_ASSERT(fd >= 0, "Failed to create Runtime tap client socket.");
    stream_fd_ = fd;
    const auto address = MakeSocketAddress(session.stream_socket);
    MW_ASSERT(
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == 0,
        "Failed to connect Runtime tap stream '{}': {}.",
        session.stream_socket, std::strerror(errno));
    RequireSameUid(fd);
    ConfigureRuntimeSocket(fd);
    reader_thread_ = std::thread(
        [this, advertisement, callback = std::move(callback)]() mutable {
          ReadLoop(advertisement, std::move(callback));
        });
  } catch (...) {
    running_ = false;
    const int fd = stream_fd_.exchange(-1);
    if (fd >= 0) {
      ::close(fd);
    }
    if (!session_id_.empty()) {
      try {
        agent_client_.StopTap(session_id_);
      } catch (...) {
      }
      session_id_.clear();
    }
    throw;
  }
}

void UnixRuntimeTapClient::StopTap() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_requested_ = true;
  if (!session_id_.empty() && !remote_closed_.load()) {
    try {
      MW_ASSERT(
          agent_client_.StopTap(session_id_),
          "Runtime Agent did not recognize tap session '{}'.", session_id_);
    } catch (const std::exception& error) {
      if (std::filesystem::exists(control_socket_)) {
        ReportStreamError(
            "Runtime tap stop request failed: " +
            std::string(error.what()));
      } else {
        remote_closed_ = true;
      }
      const int fd = stream_fd_.load();
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
      }
    } catch (...) {
      if (std::filesystem::exists(control_socket_)) {
        ReportStreamError("Runtime tap stop request failed unexpectedly.");
      } else {
        remote_closed_ = true;
      }
      const int fd = stream_fd_.load();
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
  }
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }
  const int closed_fd = stream_fd_.exchange(-1);
  if (closed_fd >= 0) {
    ::close(closed_fd);
  }
  session_id_.clear();
  std::optional<std::string> stream_error;
  {
    std::lock_guard lock(stream_error_mutex_);
    stream_error = stream_error_;
  }
  MW_ASSERT(!stream_error, "{}", stream_error.value_or(""));
}

void UnixRuntimeTapClient::ReadLoop(
    TopicAdvertisement advertisement,
    ibmw::transport::SerializedMessageCallback callback) {
  try {
    const int fd = stream_fd_.load();
    while (true) {
      TapWireHeader wire{};
      const auto header_result = ReceiveAll(fd, &wire, sizeof(wire));
      if (header_result.status == ReceiveStatus::kEof) {
        if (!stop_requested_.load() && !terminal_received_.load()) {
          remote_closed_ = true;
          MW_WARN(
              "Runtime tap stream for '{}' closed at a frame boundary; "
              "waiting for discovery to remove or replace the endpoint.",
              advertisement.topic.topic);
        }
        break;
      }
      if (header_result.status != ReceiveStatus::kOk) {
        if (stop_requested_.load()) {
          break;
        }
        if (header_result.status == ReceiveStatus::kTruncated) {
          ReportStreamError(
              "Runtime tap stream for '" + advertisement.topic.topic +
              "' ended with a partial frame header.");
        } else {
          ReportStreamError(
              "Runtime tap stream receive failed for '" +
              advertisement.topic.topic + "': " +
              std::system_category().message(
                  header_result.error_number));
        }
        break;
      }
      const uint32_t magic = ntohl(wire.magic);
      const uint32_t protocol_version = ntohl(wire.protocol_version);
      const uint32_t json_size = ntohl(wire.json_size);
      const uint64_t payload_size = NetworkToHost64(wire.payload_size);
      MW_ASSERT(
          magic == kRuntimeTapMagic, "Invalid Runtime tap stream magic.");
      MW_ASSERT(
          protocol_version == kRuntimeProtocolVersion,
          "Unsupported Runtime tap stream protocol version {}.",
          protocol_version);
      MW_ASSERT(
          wire.reserved == 0, "Runtime tap stream reserved field is nonzero.");
      MW_ASSERT(
          json_size <= kRuntimeMaxStreamHeader,
          "Runtime tap header is too large.");
      MW_ASSERT(
          payload_size <= kRuntimeMaxStreamPayload,
          "Runtime tap payload is too large.");

      std::string json_data(json_size, '\0');
      std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
      const auto json_result =
          ReceiveAll(fd, json_data.data(), json_data.size());
      const auto payload_result =
          json_result.status == ReceiveStatus::kOk
              ? ReceiveAll(fd, payload.data(), payload.size())
              : ReceiveResult{.status = ReceiveStatus::kTruncated};
      if (json_result.status != ReceiveStatus::kOk ||
          payload_result.status != ReceiveStatus::kOk) {
        if (stop_requested_.load()) {
          break;
        }
        const auto& failure = json_result.status != ReceiveStatus::kOk
                                  ? json_result
                                  : payload_result;
        if (failure.status == ReceiveStatus::kError) {
          ReportStreamError(
              "Runtime tap stream body receive failed for '" +
              advertisement.topic.topic + "': " +
              std::system_category().message(failure.error_number));
        } else {
          ReportStreamError(
              "Runtime tap stream for '" + advertisement.topic.topic +
              "' ended with a partial frame body.");
        }
        break;
      }
      const auto metadata = nlohmann::json::parse(json_data);
      const std::string kind = metadata.value("kind", "message");
      const uint64_t dropped_total = metadata.value("dropped_total", 0ull);
      ReportDroppedTotal(dropped_total);
      if (kind == "dropped") {
        MW_ASSERT(
            payload.empty(), "Runtime tap drop frame contains a payload.");
        continue;
      }
      if (kind == "terminal") {
        MW_ASSERT(
            payload.empty(), "Runtime tap terminal frame contains a payload.");
        terminal_received_ = true;
        const bool ok = metadata.value("ok", false);
        const std::string terminal_error = metadata.value("error", "");
        if (!ok) {
          ReportStreamError(
              terminal_error.empty()
                  ? "Runtime tap stream reported an unspecified failure."
                  : terminal_error);
        } else if (!stop_requested_.load()) {
          remote_closed_ = true;
        }
        break;
      }
      MW_ASSERT(kind == "message", "Unknown Runtime tap frame kind '{}'.", kind);
      callback(ibmw::transport::SerializedMessage{
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
          .payload = MakeOwnedPayload(std::move(payload))});
    }
  } catch (const std::exception& error) {
    ReportStreamError(
        "Runtime tap stream failed for '" + advertisement.topic.topic +
        "': " + error.what());
  } catch (...) {
    ReportStreamError(
        "Runtime tap stream failed for '" + advertisement.topic.topic +
        "' with an unknown error.");
  }
}

void UnixRuntimeTapClient::ReportDroppedTotal(uint64_t dropped_total) {
  MW_ASSERT(
      dropped_total >= reported_dropped_total_,
      "Runtime tap dropped-message counter regressed from {} to {}.",
      reported_dropped_total_, dropped_total);
  const uint64_t delta = dropped_total - reported_dropped_total_;
  reported_dropped_total_ = dropped_total;
  if (delta == 0) {
    return;
  }
  if (dropped_message_callback_) {
    dropped_message_callback_(delta);
  } else {
    MW_WARN(
        "Runtime tap reported {} dropped message(s), but no accounting callback is installed.",
        delta);
  }
}

void UnixRuntimeTapClient::ReportStreamError(std::string error) {
  bool first_error = false;
  {
    std::lock_guard lock(stream_error_mutex_);
    if (!stream_error_) {
      stream_error_ = error;
      first_error = true;
    }
  }
  if (!first_error) {
    return;
  }
  const int fd = stream_fd_.load();
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
  }
  MW_ERROR("{}", error);
  if (stream_error_callback_) {
    try {
      stream_error_callback_(error);
    } catch (const std::exception& callback_error) {
      MW_ERROR(
          "Runtime tap stream-error callback failed: {}",
          callback_error.what());
    } catch (...) {
      MW_ERROR("Runtime tap stream-error callback failed unexpectedly.");
    }
  }
}

}  // namespace ibmw::extensions::record_playback_extension
