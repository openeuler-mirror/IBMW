// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "engine/engine.h"

#include "cpp/core.h"
#include "type_support/ros2/ros2_transport.h"

#include "example_ros2/msg/ros_test_msg.hpp"

using namespace ibmw::runtime::core;

// SIGINT/SIGTERM only write the signal number to a pipe (async-signal-safe);
// a watcher thread calls Engine::Shutdown(), which logs and locks.
std::mutex g_core_mutex;
Engine* global_core_ptr = nullptr;
int g_signal_pipe[2] = {-1, -1};

// Makes `core` the target of SIGINT/SIGTERM while in scope.
class ScopedSignalCore {
 public:
  explicit ScopedSignalCore(Engine* core) { Set(core); }
  ~ScopedSignalCore() { Set(nullptr); }
  ScopedSignalCore(const ScopedSignalCore&) = delete;
  ScopedSignalCore& operator=(const ScopedSignalCore&) = delete;

 private:
  static void Set(Engine* core) {
    std::lock_guard<std::mutex> lck(g_core_mutex);
    global_core_ptr = core;
  }
};

void SignalHandler(int sig) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(sig);
  [[maybe_unused]] auto ret = write(g_signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

bool InstallSignalHandlers() {
  if (pipe2(g_signal_pipe, O_CLOEXEC) != 0 ||
      fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
    return false;
  }
  std::thread([] {
    unsigned char sig = 0;
    while (true) {
      const ssize_t n = read(g_signal_pipe[0], &sig, 1);
      if (n != 1) {
        if (n < 0 && errno == EINTR) continue;
        return;
      }
      std::lock_guard<std::mutex> lck(g_core_mutex);
      if (global_core_ptr == nullptr) {
        // No engine (anymore): die from the signal.
        std::signal(sig, SIG_DFL);
        raise(sig);
        continue;
      }
      global_core_ptr->Shutdown();
    }
  }).detach();
  return std::signal(SIGINT, SignalHandler) != SIG_ERR &&
         std::signal(SIGTERM, SignalHandler) != SIG_ERR;
}

int32_t main(int32_t argc, char** argv) {
  if (!InstallSignalHandlers()) {
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }

  std::cout << "Ibmw start." << std::endl;

  try {
    Engine core;
    ScopedSignalCore signal_core(&core);

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("NormalSubscriberModule"));

    std::string topic_name = "test_topic";

    // Read cfg
    auto file_path = module_handle.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name = cfg_node["topic"].as<std::string>();
    }

    // Subscribe
    auto subscriber = module_handle.GetTransport().GetSubscriber(topic_name);
    MW_CHECK_LOG_THROW(module_handle.GetLogger(),
                               subscriber, ibmw::common::util::kLogLevelError, "Get subscriber for topic '{}' failed.", topic_name);

    bool ret = ibmw::transport::OnMessage<example_ros2::msg::RosTestMsg>(
        subscriber,
        [module_handle](const std::shared_ptr<const example_ros2::msg::RosTestMsg>& data) {
          MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo,
                        "Receive new ros event, data:\n{}", example_ros2::msg::to_yaml(*data));
        });
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), ret, ibmw::common::util::kLogLevelError, "Subscribe failed.");

    // Start
    core.Start();

    // Shutdown
    core.Shutdown();
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
