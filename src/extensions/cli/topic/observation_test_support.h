// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "topic_command.h"
#include "engine/runtime_graph/runtime_graph_agent.h"
#include "engine/topic_io/runtime_tap_stream.h"
#include "gtest/gtest.h"

namespace ibmw::cli::topic::test {
namespace graph = ibmw::runtime::core::runtime_graph;
namespace io = ibmw::runtime::core::topic_io;

struct CommandResult { int code; std::string output; std::string error; };
inline CommandResult Invoke(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "topic");
  std::vector<const char*> argv;
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int code = RunTopicCommand(static_cast<int>(argv.size()), argv.data());
  return {code, testing::internal::GetCapturedStdout(), testing::internal::GetCapturedStderr()};
}

class TestTap final : public io::PublishTapCapability {
 public:
  uint64_t Register(Callback callback) override {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    return 1;
  }
  // Like the runtime tap, Unregister waits for an in-flight Emit, and the
  // callback runs without the lock Register takes: RuntimeTapServer registers
  // while holding its session lock, which OnPublish also takes.
  bool Unregister(uint64_t) override {
    std::lock_guard emit_lock(emit_mutex_);
    std::lock_guard lock(mutex_);
    callback_ = {};
    return true;
  }
  bool Emit(std::string_view bytes = "payload", std::string_view serialization = "cdr") {
    std::lock_guard emit_lock(emit_mutex_);
    Callback callback;
    {
      std::lock_guard lock(mutex_);
      callback = callback_;
    }
    if (!callback) return false;
    ibmw::transport::Context context;
    ibmw::extension::MessageView view{
        .topic = "/rate", .msg_type = "test:Bytes", .module_name = "publisher", .endpoint_index = 0,
        .context = ibmw::transport::ContextRef(context),
        .available_serialization_types = {std::string(serialization)},
        .acquire_serialized = [&](std::string_view) {
          return std::make_shared<ibmw::util::BufferArrayView>(bytes.data(), bytes.size());
        }};
    callback(view);
    return true;
  }
 private:
  std::mutex emit_mutex_;
  std::mutex mutex_;
  Callback callback_;
};

class RegistryObservationTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-observation-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory = pattern;
    server = std::make_unique<io::RuntimeTapServer>(tap, "observation-test", directory);
    server->Start();
    graph::RuntimeRegistration registration{.runtime_id = "observation-test"};
    registration.topics.push_back({.publisher_id = "publisher-0",
        .topic = {.topic = "/rate", .msg_type = "test:Bytes", .serialization_type = "cdr"},
        .capture_capabilities = {{"runtime-tap", {.mode = "runtime_tap"}}}});
    RestartAgent(registration);
  }
  void RestartAgent(const graph::RuntimeRegistration& registration) {
    agent.reset();
    agent = std::make_unique<graph::RuntimeGraphAgent>(graph::RuntimeGraphAgent::Options{
        .registration = registration, .registry_path = directory});
    agent->SetStartTapHandler([&](const auto& advertisement) { return server->StartSession(advertisement); });
    agent->SetStopTapHandler([&](auto id) { return server->StopSession(id); });
    agent->Initialize();
    agent->Start();
  }
  void TearDown() override {
    agent.reset();
    server.reset();
    std::filesystem::remove_all(directory);
  }
  std::filesystem::path directory;
  TestTap tap;
  std::unique_ptr<io::RuntimeTapServer> server;
  std::unique_ptr<graph::RuntimeGraphAgent> agent;
};

}  // namespace ibmw::cli::topic::test
