// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <csignal>
#include <iostream>

#include "engine/engine.h"

#include "cpp/core.h"
#include "type_support/ros2/ros2_transport.h"

#include "example_ros2/msg/ros_test_msg.hpp"

using namespace ibmw::runtime::core;

bool run_flag = true;

void SignalHandler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    run_flag = false;
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

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("NormalPublisherModule"));

    std::string topic_name = "test_topic";
    double transport_frq = 0.5;

    // Read cfg
    auto file_path = module_handle.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name = cfg_node["topic"].as<std::string>();
      transport_frq = cfg_node["transport_frq"].as<double>();
    }

    // Register publish type
    auto publisher = module_handle.GetTransport().GetPublisher(topic_name);
    MW_CHECK_LOG_THROW(module_handle.GetLogger(),
                               publisher, ibmw::common::util::kLogLevelError, "Get publisher for topic '{}' failed.", topic_name);

    bool ret = ibmw::transport::RegisterSendType<example_ros2::msg::RosTestMsg>(publisher);
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), ret, ibmw::common::util::kLogLevelError, "Register publishType failed.");

    ibmw::transport::SenderAccess<example_ros2::msg::RosTestMsg> publisher_access(publisher);

    // Start
    auto fu = core.AsyncStart();

    // Publish event
    uint32_t count = 0;
    while (run_flag) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<uint32_t>(1000 / transport_frq)));

      count++;
      MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "Loop count : {} -------------------------", count);

      // publish ros event
      example_ros2::msg::RosTestMsg msg;
      msg.data = {1, 2, 3, 4};
      msg.num = count + 1000;

      MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo,
                    "Publish new ros event, data:\n{}", example_ros2::msg::to_yaml(msg));
      publisher_access.Send(msg);
    }

    // Shutdown
    core.Shutdown();

    // Wait
    fu.wait();

  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
