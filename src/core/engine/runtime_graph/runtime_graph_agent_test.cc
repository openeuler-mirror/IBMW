// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "engine/runtime_graph/runtime_graph_agent.h"
#include "engine/runtime_graph/runtime_graph_client.h"
#include "engine/runtime_graph/runtime_protocol.h"

namespace ibmw::runtime::core::runtime_graph {
namespace {

using namespace std::chrono_literals;

class RuntimeGraphAgentTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string path = "/tmp/ibmw-agent-test-XXXXXX";
    ASSERT_NE(::mkdtemp(path.data()), nullptr);
    directory_ = path;
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::filesystem::path directory_;
};

TEST_F(RuntimeGraphAgentTest, ServesRequestsAndStopsTapAfterIdleTimeout) {
  RuntimeRegistration registration{.runtime_id = "idle-agent"};
  registration.topics.push_back({
      .publisher_id = "publisher", .topic = {.topic = "/test", .msg_type = "test:Message"}});
  RuntimeGraphAgent agent({.registration = registration, .registry_path = directory_});
  agent.SetStartTapHandler([](const TopicAdvertisement&) {
    return TapSessionDescriptor{.session_id = "test-session", .stream_socket = "/unused"};
  });
  agent.SetStopTapHandler([](std::string_view session_id) {
    return session_id == "test-session";
  });
  agent.Initialize();
  agent.Start();
  RuntimeGraphAgentClient client(agent.Registration().control_socket);
  ASSERT_EQ(client.ListTopics().size(), 1u);

  // Control traffic may be idle even while a separate Tap stream is active.
  std::this_thread::sleep_for(kRuntimeSocketTimeout + 1s);
  ASSERT_EQ(client.ListTopics().size(), 1u);
  const auto session = client.StartTap("/test", "publisher");
  EXPECT_EQ(session.session_id, "test-session");

  std::this_thread::sleep_for(kRuntimeSocketTimeout + 1s);
  EXPECT_TRUE(client.StopTap(session.session_id));
  EXPECT_EQ(client.GetTopicDescriptor("/test", "publisher").topic.topic, "/test");
}

TEST_F(RuntimeGraphAgentTest, IncompleteRequestTimesOutWithoutStoppingService) {
  RuntimeGraphAgent agent({.registration = {.runtime_id = "request-agent"},
                           .registry_path = directory_});
  agent.Initialize();
  agent.Start();

  struct Socket {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ~Socket() {
      if (fd >= 0) ::close(fd);
    }
  } socket;
  ASSERT_GE(socket.fd, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto& path = agent.Registration().control_socket;
  ASSERT_LT(path.size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  ASSERT_EQ(::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)), 0);
  ConfigureRuntimeSocket(socket.fd);
  const std::string incomplete_request = "{\"method\":";
  ASSERT_EQ(::send(socket.fd, incomplete_request.data(), incomplete_request.size(),
                   MSG_NOSIGNAL), static_cast<ssize_t>(incomplete_request.size()));

  // Accepted sockets must still bound stalled clients after the listener fix.
  pollfd pending{.fd = socket.fd, .events = POLLIN, .revents = 0};
  const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      kRuntimeSocketTimeout + 2s).count();
  ASSERT_EQ(::poll(&pending, 1, static_cast<int>(deadline_ms)), 1);
  std::string response;
  while (response.find('\n') == std::string::npos) {
    char buffer[1024];
    const auto received = ::recv(socket.fd, buffer, sizeof(buffer), 0);
    ASSERT_GT(received, 0);
    response.append(buffer, static_cast<size_t>(received));
    ASSERT_LT(response.size(), kRuntimeMaxControlMessage);
  }
  EXPECT_FALSE(nlohmann::json::parse(response).at("ok").get<bool>());

  RuntimeGraphAgentClient client(agent.Registration().control_socket);
  EXPECT_TRUE(client.ListTopics().empty());
}

TEST_F(RuntimeGraphAgentTest, ShutdownWakesIdleListenerAndRemovesRegistration) {
  RuntimeGraphAgent agent({.registration = {.runtime_id = "shutdown-agent"},
                           .registry_path = directory_});
  agent.Initialize();
  agent.Start();
  const auto socket_path = agent.Registration().control_socket;
  ASSERT_TRUE(RuntimeGraphAgentClient(socket_path).ListTopics().empty());

  const auto started = std::chrono::steady_clock::now();
  agent.Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  EXPECT_FALSE(std::filesystem::exists(
      ibmw::extension::RuntimeRegistryDirectory(directory_) / "shutdown-agent.json"));
}

}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
