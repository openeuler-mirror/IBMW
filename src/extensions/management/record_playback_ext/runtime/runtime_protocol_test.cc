// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/runtime/bag_runtime_agent.h"
#include "record_playback_ext/runtime/bag_runtime_agent_client.h"
#include "record_playback_ext/runtime/runtime_playback_stream.h"
#include "record_playback_ext/runtime/runtime_protocol.h"
#include "record_playback_ext/runtime/runtime_registry.h"
#include "record_playback_ext/runtime/runtime_tap_stream.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

class RuntimeTemporaryDirectory {
 public:
  RuntimeTemporaryDirectory() {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("ibmw-runtime-protocol-" + std::to_string(::getpid()) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~RuntimeTemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

int ConnectUnixSocket(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.string().size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::memcpy(
      address.sun_path, path.c_str(), path.string().size() + 1);
  if (::connect(
          fd, reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool SendExactly(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t sent =
        ::send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
    if (sent <= 0) {
      return false;
    }
    offset += static_cast<size_t>(sent);
  }
  return true;
}

bool ReceiveExactly(int fd, void* data, size_t size) {
  auto* bytes = static_cast<uint8_t*>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
    if (received <= 0) {
      return false;
    }
    offset += static_cast<size_t>(received);
  }
  return true;
}

uint64_t NetworkToHost64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return (static_cast<uint64_t>(ntohl(static_cast<uint32_t>(value))) << 32) |
         ntohl(static_cast<uint32_t>(value >> 32));
#else
  return value;
#endif
}

TopicAdvertisement TestAdvertisement(std::string runtime_id) {
  const std::string schema_data = "struct RuntimeMessage { octet value; };";
  return TopicAdvertisement{
      .runtime_id = std::move(runtime_id),
      .publisher_id = "publisher-0",
      .topic =
          {.topic = "runtime_protocol_test",
           .msg_type = "dds:RuntimeMessage",
           .serialization_type = "cdr",
           .schema =
               {.type_name = "dds:RuntimeMessage",
                .serialization_type = "cdr",
                .schema_name = "RuntimeMessage",
                .schema_encoding = "omgidl",
                .schema_data = schema_data,
                .schema_hash = ibmw::util::ComputeSchemaHash(schema_data)}}};
}

mode_t Permissions(const std::filesystem::path& path) {
  struct stat value {};
  EXPECT_EQ(::lstat(path.c_str(), &value), 0);
  return value.st_mode & 0777;
}

TEST(RuntimeProtocolTest, RegistrationRequiresV1AndOwnerOnlyPermissions) {
  RuntimeTemporaryDirectory temporary;
  const auto registration_path = temporary.Path() / "runtime.json";
  const auto start_time = ReadProcessStartTimeTicks(::getpid());
  ASSERT_TRUE(start_time.has_value());
  RuntimeRegistration registration{
      .runtime_id = "runtime",
      .pid = ::getpid(),
      .process_start_time_ticks = *start_time,
      .control_socket = (temporary.Path() / "runtime.sock").string()};
  WriteRuntimeRegistration(registration, registration_path);

  EXPECT_EQ(Permissions(temporary.Path()), 0700);
  EXPECT_EQ(Permissions(registration_path), 0600);
  std::ifstream input(registration_path);
  const auto json = nlohmann::json::parse(input);
  EXPECT_EQ(json.at("protocol_version"), kRuntimeProtocolVersion);
  EXPECT_EQ(
      RuntimeRegistrationFromJson(json).protocol_version,
      kRuntimeProtocolVersion);

  auto missing = json;
  missing.erase("protocol_version");
  EXPECT_THROW(
      RuntimeRegistrationFromJson(missing),
      ibmw::common::util::IbmwException);
  auto future = json;
  future["protocol_version"] = kRuntimeProtocolVersion + 1;
  EXPECT_THROW(
      RuntimeRegistrationFromJson(future),
      ibmw::common::util::IbmwException);
}

TEST(RuntimeProtocolTest, DiscoveryRemovesAStaleSameUidRegistration) {
  RuntimeTemporaryDirectory temporary;
  std::filesystem::permissions(
      temporary.Path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  const auto socket_path = temporary.Path() / "stale.sock";
  const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(socket_fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(
      address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
  ASSERT_EQ(
      ::bind(
          socket_fd, reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)),
      0);
  std::filesystem::permissions(
      socket_path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  const auto start_time = ReadProcessStartTimeTicks(::getpid());
  ASSERT_TRUE(start_time.has_value());
  const auto registration_path = temporary.Path() / "stale.json";
  WriteRuntimeRegistration(
      RuntimeRegistration{
          .runtime_id = "stale",
          .pid = ::getpid(),
          .process_start_time_ticks = *start_time + 1,
          .control_socket = socket_path.string()},
      registration_path);

  EXPECT_TRUE(RuntimeRegistry(temporary.Path()).DiscoverLocalRuntimes().empty());
  EXPECT_FALSE(std::filesystem::exists(registration_path));
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  ::close(socket_fd);
}

TEST(RuntimeProtocolTest, DiscoveryRejectsAControlSocketOutsideRegistry) {
  RuntimeTemporaryDirectory registry;
  RuntimeTemporaryDirectory outside;
  std::filesystem::permissions(
      registry.Path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  const auto outside_socket = outside.Path() / "runtime.sock";
  const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(socket_fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(outside_socket.string().size(), sizeof(address.sun_path));
  std::memcpy(
      address.sun_path, outside_socket.c_str(),
      outside_socket.string().size() + 1);
  ASSERT_EQ(
      ::bind(
          socket_fd, reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)),
      0);
  std::filesystem::permissions(
      outside_socket,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  const auto start_time = ReadProcessStartTimeTicks(::getpid());
  ASSERT_TRUE(start_time.has_value());
  const auto registration_path = registry.Path() / "runtime.json";
  const auto registration = RuntimeRegistration{
      .runtime_id = "runtime",
      .pid = ::getpid(),
      .process_start_time_ticks = *start_time,
      .control_socket = outside_socket.string()};
  std::ofstream(registration_path)
      << RuntimeRegistrationToJson(registration).dump() << '\n';
  std::filesystem::permissions(
      registration_path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  EXPECT_TRUE(RuntimeRegistry(registry.Path()).DiscoverLocalRuntimes().empty());
  EXPECT_FALSE(std::filesystem::exists(registration_path));
  EXPECT_TRUE(std::filesystem::exists(outside_socket));
  ::close(socket_fd);
}

TEST(RuntimeProtocolTest, AgentRejectsUnsafeRuntimeId) {
  RuntimeTemporaryDirectory temporary;
  BagRuntimeAgent agent(BagRuntimeAgent::Options{
      .registration = RuntimeRegistration{.runtime_id = "../escape"},
      .registry_path = temporary.Path()});
  EXPECT_THROW(agent.Initialize(), ibmw::common::util::IbmwException);
  EXPECT_FALSE(std::filesystem::exists(temporary.Path().parent_path() /
                                       "escape.json"));
}

TEST(RuntimeProtocolTest, DiscoveryRemovesOversizedRegistration) {
  RuntimeTemporaryDirectory temporary;
  std::filesystem::permissions(
      temporary.Path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  const auto registration_path = temporary.Path() / "oversized.json";
  {
    std::ofstream output(registration_path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.seekp(static_cast<std::streamoff>(kRuntimeMaxControlMessage));
    output.put('\0');
  }
  std::filesystem::permissions(
      registration_path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  EXPECT_TRUE(RuntimeRegistry(temporary.Path()).DiscoverLocalRuntimes().empty());
  EXPECT_FALSE(std::filesystem::exists(registration_path));
}

TEST(RuntimeProtocolTest, DiscoveryRemovesCorruptRegistrationAndOwnedSocket) {
  RuntimeTemporaryDirectory temporary;
  std::filesystem::permissions(
      temporary.Path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  const auto registration_path = temporary.Path() / "corrupt.json";
  std::ofstream(registration_path) << "{not-json\n";
  const auto socket_path = temporary.Path() / "corrupt.sock";
  const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(socket_fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(
      address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
  ASSERT_EQ(
      ::bind(
          socket_fd, reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)),
      0);
  std::filesystem::permissions(
      registration_path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      socket_path,
      std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  EXPECT_TRUE(RuntimeRegistry(temporary.Path()).DiscoverLocalRuntimes().empty());
  EXPECT_FALSE(std::filesystem::exists(registration_path));
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  ::close(socket_fd);
}

TEST(RuntimeProtocolTest, ControlRpcRejectsAnUnknownVersion) {
  RuntimeTemporaryDirectory temporary;
  BagRuntimeAgent agent(BagRuntimeAgent::Options{
      .registration = RuntimeRegistration{.runtime_id = "agent"},
      .registry_path = temporary.Path()});
  ASSERT_NO_THROW(agent.Initialize());
  agent.Start();
  EXPECT_EQ(Permissions(temporary.Path()), 0700);
  EXPECT_EQ(Permissions(agent.Registration().control_socket), 0600);
  EXPECT_TRUE(
      BagRuntimeAgentClient(agent.Registration().control_socket)
          .ListTopics()
          .empty());

  const int fd = ConnectUnixSocket(agent.Registration().control_socket);
  ASSERT_GE(fd, 0);
  const std::string request = nlohmann::json{
      {"protocol_version", kRuntimeProtocolVersion + 1},
      {"id", 7},
      {"method", "ListTopics"}}
                                  .dump() +
                              "\n";
  ASSERT_TRUE(SendExactly(fd, request.data(), request.size()));
  std::string response_data;
  char byte = 0;
  while (::recv(fd, &byte, 1, 0) == 1 && byte != '\n') {
    response_data.push_back(byte);
  }
  ::close(fd);
  const auto response = nlohmann::json::parse(response_data);
  EXPECT_EQ(response.at("protocol_version"), kRuntimeProtocolVersion);
  EXPECT_FALSE(response.at("ok").get<bool>());
  EXPECT_NE(
      response.at("error").get<std::string>().find("protocol version"),
      std::string::npos);
  agent.Shutdown();
}

TEST(RuntimeProtocolTest, TapFramesCarryV1AndOwnerOnlySocket) {
  RuntimeTemporaryDirectory temporary;
  ibmw::runtime::core::transport::TransportBus transport_bus;
  transport_bus.Initialize(YAML::Load(R"(
drivers:
  - type: local
publishers:
  - topic: runtime_protocol_test
    enable_drivers: [local]
)"));
  transport_bus.Start();
  RuntimeTapServer server(transport_bus, "tap-runtime", temporary.Path());
  server.Start();
  const auto session = server.StartSession(TestAdvertisement("tap-runtime"));
  EXPECT_EQ(Permissions(session.stream_socket), 0600);
  const int fd = ConnectUnixSocket(session.stream_socket);
  ASSERT_GE(fd, 0);

  const std::string payload = "tap-payload";
  auto payload_view = std::make_shared<ibmw::util::BufferArrayView>(
      payload.data(), payload.size());
  ibmw::transport::Context context;
  ibmw::runtime::core::transport::MessageFrame frame{
      .info =
          {.msg_type = "dds:RuntimeMessage",
           .topic_name = "runtime_protocol_test",
           .pkg_path = "test",
           .module_name = "publisher",
           .index = 0},
      .ctx_ref = ibmw::transport::ContextRef(context)};
  frame.serialization_cache.emplace("cdr", payload_view);
  transport_bus.Send(std::move(frame));

  RuntimeStreamHeader header{};
  ASSERT_TRUE(ReceiveExactly(fd, &header, sizeof(header)));
  EXPECT_EQ(ntohl(header.magic), kRuntimeTapMagic);
  EXPECT_EQ(ntohl(header.protocol_version), kRuntimeProtocolVersion);
  EXPECT_EQ(header.reserved, 0U);
  const uint32_t json_size = ntohl(header.json_size);
  const uint64_t payload_size = NetworkToHost64(header.payload_size);
  ASSERT_LE(json_size, kRuntimeMaxStreamHeader);
  ASSERT_EQ(payload_size, payload.size());
  std::string metadata(json_size, '\0');
  std::string received_payload(payload_size, '\0');
  ASSERT_TRUE(ReceiveExactly(fd, metadata.data(), metadata.size()));
  ASSERT_TRUE(ReceiveExactly(
      fd, received_payload.data(), received_payload.size()));
  EXPECT_EQ(received_payload, payload);
  EXPECT_EQ(nlohmann::json::parse(metadata).at("kind"), "message");

  auto stop = std::async(std::launch::async, [&] {
    return server.StopSession(session.session_id);
  });
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
  ASSERT_EQ(stop.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(stop.get());
  server.Shutdown();
  transport_bus.Shutdown();
}

TEST(RuntimeProtocolTest, PlaybackRejectsAnUnknownStreamVersion) {
  RuntimeTemporaryDirectory temporary;
  RuntimePlaybackServer server(
      "play-runtime", temporary.Path(),
      [](const auto&, auto&&) { ADD_FAILURE() << "message was accepted"; });
  server.Start();
  const auto session =
      server.StartSession(TestAdvertisement("play-runtime"));
  EXPECT_EQ(Permissions(session.stream_socket), 0600);
  const int fd = ConnectUnixSocket(session.stream_socket);
  ASSERT_GE(fd, 0);
  RuntimeStreamHeader frame{
      .magic = htonl(kRuntimePlaybackMagic),
      .protocol_version = htonl(kRuntimeProtocolVersion + 1),
      .json_size = 0,
      .reserved = 0,
      .payload_size = 0};
  ASSERT_TRUE(SendExactly(fd, &frame, sizeof(frame)));
  ASSERT_EQ(::shutdown(fd, SHUT_WR), 0);

  RuntimeTerminalHeader terminal{};
  ASSERT_TRUE(ReceiveExactly(fd, &terminal, sizeof(terminal)));
  EXPECT_EQ(ntohl(terminal.magic), kRuntimePlaybackTerminalMagic);
  EXPECT_EQ(ntohl(terminal.protocol_version), kRuntimeProtocolVersion);
  const uint32_t json_size = ntohl(terminal.json_size);
  ASSERT_LE(json_size, kRuntimeMaxStreamHeader);
  std::string body(json_size, '\0');
  ASSERT_TRUE(ReceiveExactly(fd, body.data(), body.size()));
  const auto result = nlohmann::json::parse(body);
  EXPECT_FALSE(result.at("ok").get<bool>());
  EXPECT_NE(
      result.at("error").get<std::string>().find("protocol version"),
      std::string::npos);
  ::close(fd);
  EXPECT_TRUE(server.StopSession(session.session_id));
  server.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
