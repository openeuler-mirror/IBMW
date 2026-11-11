// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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
    std::string path = (std::filesystem::temp_directory_path() / "ibmw-agent-test-XXXXXX").string();
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

// Answers one StopTap request with the given id after a delay.
class FakeAgent {
 public:
  FakeAgent(const std::filesystem::path& path, nlohmann::json id, std::chrono::milliseconds delay)
      : path_(path.string()) {
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{.sun_family = AF_UNIX};
    EXPECT_LT(path_.size(), sizeof(address.sun_path)) << "TMPDIR too long for a socket path";
    std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1);
    EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    EXPECT_EQ(::listen(listen_fd_, 1), 0);
    thread_ = std::thread([this, id = std::move(id), delay] {
      const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) return;
      std::string request;
      char c = 0;
      while (::recv(fd, &c, 1, 0) == 1 && c != '\n') request.push_back(c);
      std::this_thread::sleep_for(delay);
      const std::string response =
          nlohmann::json{{"protocol_version", kRuntimeProtocolVersion}, {"id", id},
                         {"ok", true}, {"result", {{"stopped", true}}}}
              .dump() +
          "\n";
      ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
      ::close(fd);
    });
  }
  ~FakeAgent() {
    thread_.join();
    ::close(listen_fd_);
  }
  const std::string& Path() const { return path_; }

 private:
  std::string path_;
  int listen_fd_ = -1;
  std::thread thread_;
};

// COMM-028: a signal arriving while the client waits for the answer does not
// fail the request.
TEST_F(RuntimeGraphAgentTest, ClientRetriesInterruptedReceive) {
  struct sigaction action {};
  action.sa_handler = [](int) {};
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // no SA_RESTART: recv returns EINTR
  struct sigaction previous {};
  ASSERT_EQ(::sigaction(SIGUSR1, &action, &previous), 0);

  FakeAgent fake(directory_ / "fake.sock", 1, 500ms);
  const pthread_t caller = ::pthread_self();
  std::thread interrupter([caller] {
    std::this_thread::sleep_for(150ms);
    ::pthread_kill(caller, SIGUSR1);
  });
  bool stopped = false;
  EXPECT_NO_THROW(stopped = RuntimeGraphAgentClient(fake.Path()).StopTap("s"));
  EXPECT_TRUE(stopped);
  interrupter.join();
  ::sigaction(SIGUSR1, &previous, nullptr);
}

// COMM-028: an answer carrying another request id is rejected.
TEST_F(RuntimeGraphAgentTest, ClientRejectsAnswerForAnotherRequest) {
  FakeAgent fake(directory_ / "fake.sock", 2, 0ms);
  EXPECT_ANY_THROW(RuntimeGraphAgentClient(fake.Path()).StopTap("s"));
}

// Socket descriptors of this process that a child would inherit on exec.
std::set<int> InheritableSockets() {
  std::set<int> fds;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    const int fd = std::stoi(entry.path().filename().string());
    std::error_code error;
    const auto target = std::filesystem::read_symlink(entry.path(), error);
    if (error || target.string().rfind("socket:", 0) != 0) continue;
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC) == 0) fds.insert(fd);
  }
  return fds;
}

// COMM-029: the agent's listener, its accepted connections and the client's
// socket are not inherited by processes the runtime starts.
TEST_F(RuntimeGraphAgentTest, SocketsAreCloseOnExec) {
  auto expected = InheritableSockets();
  RuntimeGraphAgent agent({.registration = {.runtime_id = "cloexec-agent"},
                           .registry_path = directory_});
  agent.Initialize();
  agent.Start();
  RawClient stalled(agent.Registration().control_socket);
  expected.insert(stalled.fd);  // the test's own socket
  std::this_thread::sleep_for(200ms);  // the agent accepts and waits for the request
  EXPECT_EQ(InheritableSockets(), expected);

  FakeAgent fake(directory_ / "fake.sock", 1, 500ms);
  std::set<int> during_call;
  std::thread observer([&during_call] {
    std::this_thread::sleep_for(200ms);
    during_call = InheritableSockets();
  });
  EXPECT_TRUE(RuntimeGraphAgentClient(fake.Path()).StopTap("s"));
  observer.join();
  EXPECT_EQ(during_call, expected);
}

// COMM-027: a client dropped before its answer is delivered is no longer
// silent: it is logged as a warning, at most once per second, with a count.
TEST_F(RuntimeGraphAgentTest, DroppedClientsAreLoggedAtMostOncePerSecond) {
  std::mutex mutex;
  std::vector<std::string> warnings;
  ibmw::common::util::LoggerWrapper logger;
  logger.get_log_level_func = [] { return ibmw::common::util::kLogLevelTrace; };
  logger.log_func = [&](uint32_t level, uint32_t, const char*, const char*,
                        const char* data, size_t size) {
    if (level != ibmw::common::util::kLogLevelWarn) return;
    const std::lock_guard lock(mutex);
    warnings.emplace_back(data, size);
  };
  RuntimeGraphAgent agent({.registration = {.runtime_id = "drop-agent"},
                           .registry_path = directory_,
                           .logger = logger});
  agent.Initialize();
  agent.Start();
  const auto& path = agent.Registration().control_socket;
  const auto warning_count = [&] {
    const std::lock_guard lock(mutex);
    return warnings.size();
  };
  // Hangs up right after the request: the agent's answer fails with EPIPE.
  const auto drop_client = [&] {
    RawClient client(path);
    client.Send(R"({"protocol_version":)" + std::to_string(kRuntimeProtocolVersion) +
                R"(,"id":1,"method":"ListTopics"})" "\n");
  };

  drop_client();
  for (int i = 0; i < 100 && warning_count() == 0; ++i) std::this_thread::sleep_for(20ms);
  drop_client();
  RuntimeGraphAgentClient client(path);
  EXPECT_TRUE(client.ListTopics().empty());  // the agent has served the second drop
  ASSERT_EQ(warning_count(), 1u);
  EXPECT_EQ(warnings[0].rfind("Runtime Agent 'drop-agent' dropped a client: ", 0), 0u)
      << warnings[0];
  EXPECT_NE(warnings[0].find("Failed to send Runtime Agent response: Broken pipe."),
            std::string::npos)
      << warnings[0];
  EXPECT_NE(warnings[0].find("(1 dropped so far)"), std::string::npos) << warnings[0];

  std::this_thread::sleep_for(1100ms);
  drop_client();
  EXPECT_TRUE(client.ListTopics().empty());
  ASSERT_EQ(warning_count(), 2u);
  EXPECT_NE(warnings[1].find("(3 dropped so far)"), std::string::npos) << warnings[1];
}

}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
