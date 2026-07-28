// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/bag_runtime_agent_client.h"

#include <array>
#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/runtime/runtime_protocol.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

sockaddr_un MakeSocketAddress(const std::string& path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  MW_ASSERT(
      path.size() < sizeof(address.sun_path),
      "Runtime Agent socket path is too long: '{}'.", path);
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return address;
}

}  // namespace

std::vector<TopicAdvertisement> BagRuntimeAgentClient::ListTopics() const {
  std::vector<TopicAdvertisement> result;
  for (const auto& item : Call("ListTopics")) {
    result.emplace_back(TopicAdvertisementFromJson(item));
  }
  return result;
}

TopicAdvertisement BagRuntimeAgentClient::GetTopicDescriptor(
    std::string_view topic,
    std::string_view publisher_id) const {
  return TopicAdvertisementFromJson(Call(
      "GetTopicDescriptor",
      {{"topic", topic}, {"publisher_id", publisher_id}}));
}

TapSessionDescriptor BagRuntimeAgentClient::StartTap(
    std::string_view topic,
    std::string_view publisher_id) const {
  const auto result = Call(
      "StartTap", {{"topic", topic}, {"publisher_id", publisher_id}});
  return TapSessionDescriptor{
      .session_id = result.at("session_id").get<std::string>(),
      .stream_socket = result.at("stream_socket").get<std::string>()};
}

bool BagRuntimeAgentClient::StopTap(std::string_view session_id) const {
  return Call("StopTap", {{"session_id", session_id}})
      .at("stopped")
      .get<bool>();
}

nlohmann::json BagRuntimeAgentClient::Call(
    std::string_view method,
    nlohmann::json params) const {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  MW_ASSERT(fd >= 0, "Failed to create Runtime Agent client socket.");
  struct SocketGuard {
    int fd;
    ~SocketGuard() { ::close(fd); }
  } guard{fd};

  const auto address = MakeSocketAddress(control_socket_);
  MW_ASSERT(
      ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
      "Failed to connect Runtime Agent '{}': {}.", control_socket_,
      std::strerror(errno));
  RequireSameUid(fd);
  ConfigureRuntimeSocket(fd);
  const std::string request = nlohmann::json{
      {"protocol_version", kRuntimeProtocolVersion}, {"id", 1},
      {"method", method}, {"params", std::move(params)}}
                                  .dump() +
                              "\n";
  size_t offset = 0;
  while (offset < request.size()) {
    const ssize_t written =
        ::send(fd, request.data() + offset, request.size() - offset,
               MSG_NOSIGNAL);
    MW_ASSERT(written > 0, "Failed to send Runtime Agent request.");
    offset += static_cast<size_t>(written);
  }

  std::string response_data;
  std::array<char, 4096> buffer{};
  while (response_data.find('\n') == std::string::npos &&
         response_data.size() < kRuntimeMaxControlMessage) {
    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    MW_ASSERT(received > 0, "Runtime Agent closed without a response.");
    response_data.append(buffer.data(), static_cast<size_t>(received));
  }
  MW_ASSERT(
      !response_data.empty() &&
          response_data.size() < kRuntimeMaxControlMessage,
      "Invalid Runtime Agent response size.");
  const auto response = nlohmann::json::parse(response_data);
  MW_ASSERT(
      response.value("protocol_version", 0u) == kRuntimeProtocolVersion,
      "Unsupported Runtime Agent response protocol version.");
  MW_ASSERT(
      response.value("ok", false), "Runtime Agent request '{}' failed: {}.",
      method, response.value("error", "unknown error"));
  return response.at("result");
}

}  // namespace ibmw::extensions::record_playback_extension
