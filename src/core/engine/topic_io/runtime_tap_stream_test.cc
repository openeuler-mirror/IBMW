// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/runtime_tap_stream.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <endian.h>

#include "gtest/gtest.h"
#include "engine/transport/transport_bus.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/runtime_graph/runtime_protocol.h"
#include "engine/runtime_graph/runtime_directory.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::topic_io {
namespace {

using namespace std::chrono_literals;

class DirectPublishTap final : public PublishTapCapability {
 public:
  uint64_t Register(Callback value) override { callback = std::move(value); return 1; }
  bool Unregister(uint64_t) override { callback = {}; return true; }
  Callback callback;
};

TEST(RuntimeTapStreamTest, SharesOwnedFormatsAndRemovesPublisherIndexEntries) {
  std::string directory = "/tmp/ibmw-tap-formats-XXXXXX";
  ASSERT_NE(::mkdtemp(directory.data()), nullptr);
  DirectPublishTap tap;
  RuntimeTapServer server(tap, "runtime", directory);
  server.Start();
  TopicAdvertisement advertisement;
  advertisement.runtime_id = "runtime";
  advertisement.publisher_id = "module-7";
  advertisement.topic = {.topic = "/bytes", .msg_type = "test:Bytes", .serialization_type = "cdr"};
  std::vector<TapSessionDescriptor> sessions;
  for (int i = 0; i < 3; ++i) sessions.push_back(server.StartSession(advertisement));
  advertisement.topic.serialization_type = "json";
  sessions.push_back(server.StartSession(advertisement));
  const auto stopped = server.StartSession(advertisement);
  ASSERT_TRUE(server.StopSession(stopped.session_id));
  advertisement.publisher_id = "module-8";
  advertisement.topic.serialization_type = "unrelated";
  const auto unrelated = server.StartSession(advertisement);
  std::string cdr = "cdr-owned", json = "json-owned";
  std::map<std::string, unsigned> acquired;
  ibmw::transport::Context context;
  ibmw::extension::MessageView view{
      .topic = "/bytes", .msg_type = "test:Bytes", .module_name = "module", .endpoint_index = 7,
      .context = ibmw::transport::ContextRef(context), .available_serialization_types = {"cdr", "json"},
      .acquire_serialized = [&](std::string_view format) {
        ++acquired[std::string(format)];
        auto& bytes = format == "cdr" ? cdr : json;
        return std::make_shared<ibmw::util::BufferArrayView>(bytes.data(), bytes.size());
      }};
  tap.callback(view);
  EXPECT_EQ(acquired, (std::map<std::string, unsigned>{{"cdr", 1}, {"json", 1}}));
  cdr.assign(cdr.size(), 'x');
  json.assign(json.size(), 'x');
  // Readers connect after the borrowed storage has changed.
  for (size_t i = 0; i < sessions.size(); ++i) {
    runtime_graph::RuntimeFileDescriptor fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    ASSERT_GE(fd.Get(), 0);
    runtime_graph::ConfigureRuntimeSocket(fd.Get());
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_LT(sessions[i].stream_socket.size(), sizeof(address.sun_path));
    std::strcpy(address.sun_path, sessions[i].stream_socket.c_str());
    ASSERT_EQ(::connect(fd.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    runtime_graph::RuntimeStreamHeader header{};
    ASSERT_EQ(::recv(fd.Get(), &header, sizeof(header), MSG_WAITALL), sizeof(header));
    const size_t metadata_size = ntohl(header.json_size);
    const size_t payload_size = be64toh(header.payload_size);
    ASSERT_LT(metadata_size + payload_size, 65536u);
    std::string bytes(metadata_size + payload_size, '\0');
    ASSERT_EQ(::recv(fd.Get(), bytes.data(), bytes.size(), MSG_WAITALL), static_cast<ssize_t>(bytes.size()));
    EXPECT_EQ(bytes.substr(metadata_size), i < 3 ? "cdr-owned" : "json-owned");
  }
  for (const auto& session : sessions) EXPECT_TRUE(server.StopSession(session.session_id));
  EXPECT_TRUE(server.StopSession(unrelated.session_id));
  EXPECT_FALSE(tap.callback);
  server.Shutdown();
  std::filesystem::remove_all(directory);
}

// COMM-034: publishes are pre-filtered by topic. The filter must never hide
// a topic that still has a session, including after other sessions stop and
// with more topics than filter bits.
TEST(RuntimeTapStreamTest, TopicFilterKeepsEveryOpenSessionVisible) {
  std::string directory = "/tmp/ibmw-tap-filter-XXXXXX";
  ASSERT_NE(::mkdtemp(directory.data()), nullptr);
  DirectPublishTap tap;
  RuntimeTapServer server(tap, "runtime", directory);
  server.Start();
  TopicAdvertisement advertisement;
  advertisement.runtime_id = "runtime";
  advertisement.publisher_id = "module-7";
  advertisement.topic = {.msg_type = "test:Bytes", .serialization_type = "cdr"};
  constexpr int kTopics = 600;
  std::vector<std::string> session_ids;
  for (int i = 0; i < kTopics; ++i) {
    advertisement.topic.topic = "/t" + std::to_string(i);
    session_ids.push_back(server.StartSession(advertisement).session_id);
  }

  std::string bytes = "payload";
  unsigned acquired = 0;
  ibmw::transport::Context context;
  const auto publish = [&](const std::string& topic) {
    const unsigned before = acquired;
    ibmw::extension::MessageView view{
        .topic = topic, .msg_type = "test:Bytes", .module_name = "module", .endpoint_index = 7,
        .context = ibmw::transport::ContextRef(context), .available_serialization_types = {"cdr"},
        .acquire_serialized = [&](std::string_view) {
          ++acquired;
          return std::make_shared<ibmw::util::BufferArrayView>(bytes.data(), bytes.size());
        }};
    tap.callback(view);
    return acquired != before;
  };

  for (int i = 0; i < kTopics; ++i) EXPECT_TRUE(publish("/t" + std::to_string(i))) << i;
  // Stop every even topic: the odd ones stay visible.
  for (int i = 0; i < kTopics; i += 2) ASSERT_TRUE(server.StopSession(session_ids[i]));
  for (int i = 0; i < kTopics; ++i) {
    EXPECT_EQ(publish("/t" + std::to_string(i)), i % 2 == 1) << i;
  }
  for (int i = 1; i < kTopics; i += 2) ASSERT_TRUE(server.StopSession(session_ids[i]));
  EXPECT_FALSE(tap.callback);

  // A new session after the filter was emptied is visible again.
  advertisement.topic.topic = "/again";
  const auto again = server.StartSession(advertisement);
  EXPECT_TRUE(publish("/again"));
  EXPECT_FALSE(publish("/t1"));
  server.Shutdown();
  std::filesystem::remove_all(directory);
}

class TestPublishTap final : public PublishTapCapability {
 public:
  explicit TestPublishTap(
      ibmw::runtime::core::transport::TransportBus& transport_bus)
      : transport_bus_(transport_bus) {}

  uint64_t Register(Callback callback) override {
    return transport_bus_.RegisterPublishTap(
        [callback = std::move(callback)](
            ibmw::runtime::core::transport::MessageFrame& frame) {
          ibmw::extension::MessageView view{
              .topic = frame.info.topic_name,
              .msg_type = frame.info.msg_type,
              .module_name = frame.info.module_name,
              .endpoint_index = frame.info.index,
              .context = frame.ctx_ref,
              .has_typed_message = frame.msg_ptr != nullptr,
              .available_serialization_types = [&] {
                std::unordered_set<std::string> formats;
                for (const auto& [format, payload] : frame.serialization_cache) {
                  static_cast<void>(payload);
                  formats.emplace(format);
                }
                return formats;
              }(),
              .acquire_serialized = [&frame](std::string_view format) {
                return ibmw::runtime::core::transport::
                    TrySerializeMsgWithCache(frame, format);
              }};
          callback(view);
        });
  }
  bool Unregister(uint64_t handle) override {
    return transport_bus_.UnregisterPublishTap(handle);
  }

 private:
  ibmw::runtime::core::transport::TransportBus& transport_bus_;
};

TEST(RuntimeTapStreamTest, StopUnblocksAConnectedClientThatDoesNotRead) {
  const auto socket_directory =
      std::filesystem::temp_directory_path() /
      ("ibmw-runtime-tap-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(socket_directory);

  ibmw::runtime::core::transport::TransportBus transport_bus;
  transport_bus.Initialize(YAML::Load(R"(
drivers:
  - type: local
publishers:
  - topic: tap_stop_test
    enable_drivers: [local]
)"));
  transport_bus.Start();

  TestPublishTap tap(transport_bus);
  RuntimeTapServer server(tap, "tap-stop-runtime", socket_directory);
  server.Start();
  TopicAdvertisement advertisement;
  advertisement.runtime_id = "tap-stop-runtime";
  advertisement.publisher_id = "publisher-0";
  advertisement.topic.topic = "tap_stop_test";
  advertisement.topic.msg_type = "dds:TapStopTest";
  advertisement.topic.serialization_type = "cdr";
  const auto session = server.StartSession(advertisement);

  const int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(client_fd, 0);
  const int receive_buffer_size = 1024;
  ASSERT_EQ(
      ::setsockopt(
          client_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
          sizeof(receive_buffer_size)),
      0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(session.stream_socket.size(), sizeof(address.sun_path));
  std::memcpy(
      address.sun_path, session.stream_socket.c_str(),
      session.stream_socket.size() + 1);
  ASSERT_EQ(
      ::connect(
          client_fd, reinterpret_cast<const sockaddr*>(&address),
          sizeof(address)),
      0);

  ibmw::runtime::core::transport::TransportInfo info{
      .msg_type = "dds:TapStopTest",
      .topic_name = "tap_stop_test",
      .pkg_path = "test",
      .module_name = "publisher",
      .index = 0};
  const std::string payload(4 * 1024 * 1024, 'x');
  auto payload_view = std::make_shared<ibmw::util::BufferArrayView>(
      payload.data(), payload.size());
  for (size_t index = 0; index < 8; ++index) {
    ibmw::transport::Context context;
    ibmw::runtime::core::transport::MessageFrame frame{
        .info = info,
        .ctx_ref = ibmw::transport::ContextRef(context)};
    frame.serialization_cache.emplace("cdr", payload_view);
    transport_bus.Send(std::move(frame));
  }
  std::this_thread::sleep_for(100ms);

  std::promise<bool> stop_result;
  auto stop_future = stop_result.get_future();
  std::thread stop_thread([&] {
    try {
      stop_result.set_value(server.StopSession(session.session_id));
    } catch (...) {
      stop_result.set_exception(std::current_exception());
    }
  });

  const bool timed_out = stop_future.wait_for(2s) != std::future_status::ready;
  if (timed_out) {
    ::shutdown(client_fd, SHUT_RDWR);
  }
  const auto cleanup_status = stop_future.wait_for(2s);
  EXPECT_EQ(cleanup_status, std::future_status::ready);
  stop_thread.join();

  bool stopped = false;
  try {
    stopped = stop_future.get();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "StopSession failed: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "StopSession failed with an unknown exception";
  }

  ::close(client_fd);
  server.Shutdown();
  transport_bus.Shutdown();
  std::filesystem::remove_all(socket_directory);

  EXPECT_FALSE(timed_out) << "StopSession remained blocked in socket send";
  EXPECT_TRUE(stopped);
}

TEST(RuntimeTapStreamTest, QueueRespectsByteBudget) {
  // COMM-009: 20 x 4 MiB frames queued before the reader connects must be
  // capped by the 64 MiB byte budget (16 frames), not only by the count limit.
  std::string directory = "/tmp/ibmw-tap-budget-XXXXXX";
  ASSERT_NE(::mkdtemp(directory.data()), nullptr);
  DirectPublishTap tap;
  RuntimeTapServer server(tap, "runtime", directory);
  server.Start();
  TopicAdvertisement advertisement;
  advertisement.runtime_id = "runtime";
  advertisement.publisher_id = "module-7";
  advertisement.topic = {.topic = "/bytes", .msg_type = "test:Bytes", .serialization_type = "cdr"};
  const auto session = server.StartSession(advertisement);
  std::string bytes(4 * 1024 * 1024, 'p');
  for (int i = 0; i < 20; ++i) {
    ibmw::transport::Context context;
    ibmw::extension::MessageView view{
        .topic = "/bytes", .msg_type = "test:Bytes", .module_name = "module", .endpoint_index = 7,
        .context = ibmw::transport::ContextRef(context), .available_serialization_types = {"cdr"},
        .acquire_serialized = [&](std::string_view) {
          return std::make_shared<ibmw::util::BufferArrayView>(bytes.data(), bytes.size());
        }};
    tap.callback(view);
  }

  runtime_graph::RuntimeFileDescriptor fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_GE(fd.Get(), 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(session.stream_socket.size(), sizeof(address.sun_path));
  std::strcpy(address.sun_path, session.stream_socket.c_str());
  ASSERT_EQ(::connect(fd.Get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  timeval timeout{.tv_sec = 1, .tv_usec = 0};
  ASSERT_EQ(::setsockopt(fd.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
  size_t messages = 0;
  bool saw_drop_report = false;
  std::string frame;
  while (true) {
    runtime_graph::RuntimeStreamHeader header{};
    if (::recv(fd.Get(), &header, sizeof(header), MSG_WAITALL) != sizeof(header)) break;
    const size_t metadata_size = ntohl(header.json_size);
    const size_t payload_size = be64toh(header.payload_size);
    frame.resize(metadata_size + payload_size);
    ASSERT_EQ(::recv(fd.Get(), frame.data(), frame.size(), MSG_WAITALL), static_cast<ssize_t>(frame.size()));
    if (frame.find("\"dropped\"") < metadata_size) {
      saw_drop_report = true;
    } else {
      EXPECT_EQ(payload_size, bytes.size());
      ++messages;
    }
  }
  EXPECT_EQ(messages, 16u);
  EXPECT_TRUE(saw_drop_report);
  EXPECT_TRUE(server.StopSession(session.session_id));
  server.Shutdown();
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace ibmw::runtime::core::topic_io
