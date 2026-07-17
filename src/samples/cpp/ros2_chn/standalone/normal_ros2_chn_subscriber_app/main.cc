// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "engine/engine.h"

#include "cpp/core.h"
#include "internal/typesupport/ros2/ros2_transport.h"

#include "example_ros2/msg/ros_test_msg.hpp"

using namespace ibmw::runtime::core;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }

  raise(sig);
};

int32_t main(int32_t argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "Ibmw start." << std::endl;

  try {
    Engine core;
    global_core_ptr = &core;

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

    global_core_ptr = nullptr;
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
