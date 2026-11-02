// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
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

// A raw connection to the agent's control socket.
struct RawClient {
  explicit RawClient(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    EXPECT_LT(path.size(), sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    ConfigureRuntimeSocket(fd);
  }
  ~RawClient() { ::close(fd); }

  void Send(std::string_view data) const {
    EXPECT_EQ(::send(fd, data.data(), data.size(), MSG_NOSIGNAL), static_cast<ssize_t>(data.size()));
  }

  nlohmann::json ReadResponse() const {
    std::string response;
    while (response.find('\n') == std::string::npos) {
      char buffer[1024];
      const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
      if (received <= 0) return nullptr;
      response.append(buffer, static_cast<size_t>(received));
    }
    return nlohmann::json::parse(response);
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
};

// COMM-026: one silent or slow client holds the serial agent for at most the
// request deadline, not a socket timeout per received chunk.
TEST_F(RuntimeGraphAgentTest, StalledClientsAreCutOffAtTheRequestDeadline) {
  RuntimeGraphAgent agent({.registration = {.runtime_id = "deadline-agent"},
                           .registry_path = directory_});
  agent.Initialize();
  agent.Start();
  const auto& path = agent.Registration().control_socket;

  for (const bool drip : {false, true}) {
    RawClient stalled(path);
    std::thread sender;
    if (drip) {
      sender = std::thread([&stalled] {
        for (int i = 0; i < 12 && stalled.fd >= 0; ++i) {
          if (::send(stalled.fd, " ", 1, MSG_NOSIGNAL) != 1) return;
          std::this_thread::sleep_for(250ms);
        }
      });
    }
    const auto start = std::chrono::steady_clock::now();
    const auto response = stalled.ReadResponse();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (sender.joinable()) sender.join();
    ASSERT_TRUE(response.is_object()) << "drip=" << drip;
    EXPECT_FALSE(response.at("ok").get<bool>());
    EXPECT_NE(response.at("error").get<std::string>().find("Runtime Agent request timed out."), std::string::npos);
    EXPECT_LT(elapsed, kRuntimeRequestTimeout + 1s) << "drip=" << drip;
  }

  RuntimeGraphAgentClient client(path);
  EXPECT_TRUE(client.ListTopics().empty());
}

// COMM-026: a request cut short by the peer is rejected, not parsed.
TEST_F(RuntimeGraphAgentTest, RequestWithoutNewlineIsRejected) {
  RuntimeGraphAgent agent({.registration = {.runtime_id = "newline-agent"},
                           .registry_path = directory_});
  agent.Initialize();
  agent.Start();

  RawClient client(agent.Registration().control_socket);
  client.Send(nlohmann::json{{"protocol_version", kRuntimeProtocolVersion},
                             {"id", 1}, {"method", "ListTopics"}, {"params", nlohmann::json::object()}}
                  .dump());
  ASSERT_EQ(::shutdown(client.fd, SHUT_WR), 0);
  const auto response = client.ReadResponse();
  ASSERT_TRUE(response.is_object());
  EXPECT_FALSE(response.at("ok").get<bool>());
  EXPECT_NE(response.at("error").get<std::string>().find("Runtime Agent request ended without a newline."), std::string::npos);
}

// Holds a Runtime ID lock the way another process would.
struct HeldRuntimeIdLock {
  explicit HeldRuntimeIdLock(const std::filesystem::path& path)
      : fd(::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
    EXPECT_GE(fd, 0);
    EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
  }
  ~HeldRuntimeIdLock() { ::close(fd); }
  int fd;
};

// COMM-030: a lock holder that never lets go bounds Initialize() and leaves
// Shutdown() (and so the destructor) without an exception.
TEST_F(RuntimeGraphAgentTest, HeldRuntimeIdLockDoesNotHangInitializeOrShutdown) {
  {
    RuntimeGraphAgent agent({.registration = {.runtime_id = "locked-agent"},
                             .registry_path = directory_});
    agent.Initialize();
    agent.Start();
    const std::filesystem::path socket = agent.Registration().control_socket;
    const auto registry = socket.parent_path();
    HeldRuntimeIdLock held(registry / "locked-agent.lock");

    const auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(agent.Shutdown());
    EXPECT_LT(std::chrono::steady_clock::now() - start, kRuntimeIdLockTimeout + 1s);
    // The cleanup was skipped, not forced past the lock.
    EXPECT_TRUE(std::filesystem::exists(registry / "locked-agent.json"));
  }

  RuntimeGraphAgent agent({.registration = {.runtime_id = "init-locked-agent"},
                           .registry_path = directory_});
  const auto registry = ibmw::extension::RuntimeRegistryDirectory(directory_);
  std::filesystem::create_directories(registry);
  std::filesystem::permissions(registry, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  HeldRuntimeIdLock held(registry / "init-locked-agent.lock");
  const auto start = std::chrono::steady_clock::now();
  try {
    agent.Initialize();
    ADD_FAILURE() << "Initialize ignored the held lock";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string_view(error.what()).find("Timed out locking Runtime ID"), std::string_view::npos)
        << error.what();
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, kRuntimeIdLockTimeout + 1s);
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
