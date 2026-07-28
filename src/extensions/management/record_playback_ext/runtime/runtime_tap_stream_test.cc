// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/runtime/runtime_tap_stream.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "engine/transport/transport_bus.h"
#include "engine/transport/transport_driver_tools.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

using namespace std::chrono_literals;

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

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
