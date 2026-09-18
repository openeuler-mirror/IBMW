// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <thread>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#include "engine/runtime_graph/runtime_graph_agent.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "record_playback_ext/storage/bag_reader_factory.h"
#include "zenoh_ext/zenoh_bus.h"
#include "base/buffer_util.h"
#include "gtest/gtest.h"

namespace {
using namespace std::chrono_literals;
namespace graph = ibmw::runtime::core::runtime_graph;
namespace bag = ibmw::extensions::record_playback_extension;
using Clock = std::chrono::steady_clock;

class RecorderProcess {
 public:
  RecorderProcess(const std::filesystem::path& registry, const std::filesystem::path& output,
                  const std::filesystem::path& log) {
    const std::string registry_path = registry.string(), output_path = output.string();
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("Cannot open recorder log");
    pid_ = ::fork();
    if (pid_ == 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
      ::execl(IBMW_RECORD_TEST_EXECUTABLE, IBMW_RECORD_TEST_EXECUTABLE,
              "bag", "record", "-a", "--registry", "--registry-path", registry_path.c_str(),
              "--serialization-format", "auto", "-o", output_path.c_str(), nullptr);
      ::_exit(127);
    }
    ::close(fd);
    if (pid_ < 0) throw std::runtime_error("Cannot start recorder");
  }
  ~RecorderProcess() { Stop(); }
  int Stop() {
    if (pid_ <= 0) return status_;
    ::kill(pid_, SIGINT);
    const auto deadline = Clock::now() + 5s;
    while (::waitpid(pid_, &status_, WNOHANG) == 0) {
      if (Clock::now() > deadline) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status_, 0);
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    pid_ = -1;
    return status_;
  }
 private:
  pid_t pid_ = -1;
  int status_ = 0;
};

class BagAutoSchemaRecord : public testing::TestWithParam<bool> {};

TEST_P(BagAutoSchemaRecord, ResolvesActualZenohFormatForInitialAndLateRegistryTopics) {
  std::string pattern = "/tmp/ibmw-auto-schema-XXXXXX";
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  // Bind an ephemeral loopback port, keeping multicast out of this fixture.
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(socket, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t size = sizeof(address);
  ASSERT_EQ(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size), 0);
  const auto endpoint = "tcp/127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  ::close(socket);
  const auto server_config = directory / "server.json5";
  const auto client_config = directory / "client.json5";
  std::ofstream(server_config) << "{mode:'peer',scouting:{multicast:{enabled:false}},listen:{endpoints:['" << endpoint << "']}}";
  std::ofstream(client_config) << "{mode:'peer',scouting:{multicast:{enabled:false}},listen:{endpoints:['tcp/127.0.0.1:0']},connect:{endpoints:['" << endpoint << "']}}";
  ibmw::extensions::zenoh_extension::ZenohBus sender;
  sender.Initialize(server_config.string(), 1024 * 1024);
  ASSERT_TRUE(sender.RegisterPublisher("transport/%2Finitial/test%3AAutoSchema", false));
  ASSERT_TRUE(sender.RegisterPublisher("transport/%2Flate/test%3AAutoSchema", false));
  graph::TypeSupportRegistry schemas;
  schemas.Load(AUTO_SCHEMA_PACKAGE);
  ASSERT_FALSE(schemas.Find("test:AutoSchema"));
  const auto make_topic = [&](const std::string& name) {
    graph::TopicAdvertisement topic{
        .publisher_id = name,
        .topic = {.topic = name, .msg_type = "test:AutoSchema", .serialization_type = "cdr",
                  .available_drivers = {"zenoh"}},
        .type_support_package = AUTO_SCHEMA_PACKAGE,
        .capture_capabilities = {{"zenoh", {.abi_version = 2, .mode = "native_serialized", .priority = 1,
            .extension_name = "zenoh_extension", .extension_path = ZENOH_RECORD_EXTENSION,
            .extension_options_yaml = "native_cfg_path: " + client_config.string()}}}};
    topic.topic.schema = *schemas.FindSchema("test:AutoSchema", "cdr");
    return topic;
  };
  const auto registry = directory / "registry";
  graph::RuntimeGraphAgent initial({.registration = {.runtime_id = "initial", .topics = {make_topic("/initial")}},
                                   .registry_path = registry});
  initial.Initialize();
  const auto output = directory / "bag";
  RecorderProcess recorder(registry, output, directory / "record.log");
  const std::string format = GetParam() ? "unknown" : "json";
  std::string bytes(4 + 1 + format.size() + 1 + 2, '\0');
  ibmw::common::util::BufferOperator buffer(bytes.data(), bytes.size());
  buffer.SetUint32(bytes.size() - 4);
  buffer.SetString(format, ibmw::common::util::BufferLenType::kUInt8);
  buffer.SetUint8(0);
  buffer.SetBuffer("{}", 2);
  const auto send_for = [&](std::chrono::milliseconds duration, bool late) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
      sender.Send("transport/%2Finitial/test%3AAutoSchema", bytes.data(), bytes.size());
      if (late) sender.Send("transport/%2Flate/test%3AAutoSchema", bytes.data(), bytes.size());
      std::this_thread::sleep_for(30ms);
    }
  };
  send_for(3s, false);
  graph::RuntimeGraphAgent late({.registration = {.runtime_id = "late", .topics = {make_topic("/late")}},
                                .registry_path = registry});
  late.Initialize();
  send_for(2s, true);
  const int status = recorder.Stop();
  std::ifstream log(directory / "record.log");
  const std::string diagnostic((std::istreambuf_iterator<char>(log)), {});
  ASSERT_TRUE(WIFEXITED(status)) << diagnostic;
  if (GetParam()) {
    EXPECT_EQ(WEXITSTATUS(status), 1) << diagnostic;
    EXPECT_NE(diagnostic.find("No schema for received topic"), std::string::npos) << diagnostic;
    EXPECT_NE(diagnostic.find("unknown"), std::string::npos);
    return;
  }
  ASSERT_EQ(WEXITSTATUS(status), 0) << diagnostic;
  auto reader = bag::CreateBagReader(output);
  ASSERT_TRUE(reader->Info().closed_cleanly) << diagnostic;
  ASSERT_TRUE(reader->Info().error_message.empty());
  std::map<std::string, unsigned> counts;
  reader->ReadMessages({}, [&](bag::BagMessage&& message) {
    EXPECT_EQ(message.channel.topic.serialization_type, "json");
    EXPECT_EQ(message.channel.topic.schema.serialization_type, "json");
    EXPECT_EQ(message.channel.topic.schema.schema_hash, schemas.FindSchema("test:AutoSchema", "json")->schema_hash);
    EXPECT_EQ(message.payload->JoinToString(), "{}");
    ++counts[message.channel.topic.topic];
    return true;
  });
  EXPECT_GT(counts["/initial"], 0u) << diagnostic;
  EXPECT_GT(counts["/late"], 0u) << diagnostic;
}
INSTANTIATE_TEST_SUITE_P(EnvelopeFormats, BagAutoSchemaRecord, testing::Bool());
}  // namespace
