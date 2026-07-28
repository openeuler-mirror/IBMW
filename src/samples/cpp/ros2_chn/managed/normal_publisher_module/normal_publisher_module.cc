// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_publisher_module/normal_publisher_module.h"
#include "type_support/ros2/ros2_transport.h"

#include "yaml-cpp/yaml.h"

#include "example_ros2/msg/ros_test_msg.hpp"

namespace ibmw::examples::cpp::ros2_chn::normal_publisher_module {

bool NormalPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Read cfg
    auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name_ = cfg_node["topic"].as<std::string>();
      transport_frq_ = cfg_node["transport_frq"].as<double>();
    }

    // Get executor handle
    executor_ = core_.GetExecutor().GetExecutor("work_thread_pool");
    MW_REQUIRE(executor_ && executor_.SupportsTimedDispatch(),
                            "Get executor 'work_thread_pool' failed.");

    // Register publish type
    publisher_ = core_.GetTransport().GetPublisher(topic_name_);
    MW_REQUIRE(publisher_, "Get publisher for topic '{}' failed.", topic_name_);

    bool ret = ibmw::transport::RegisterSendType<example_ros2::msg::RosTestMsg>(publisher_);
    MW_REQUIRE(ret, "Register publishType failed.");

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool NormalPublisherModule::Run() {
  try {
    run_flag_ = true;
    executor_.Dispatch(std::bind(&NormalPublisherModule::MainLoop, this));
  } catch (const std::exception& e) {
    MW_ERROR("Start failed, {}", e.what());
    return false;
  }

  MW_INFO("Run succeeded.");
  return true;
}

void NormalPublisherModule::Stop() {
  try {
    if (run_flag_) {
      run_flag_ = false;
      stop_sig_.get_future().wait();
    }
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }

  MW_INFO("Stop succeeded.");
}

// Main loop
void NormalPublisherModule::MainLoop() {
  try {
    MW_INFO("Start MainLoop.");

    ibmw::transport::SenderAccess<example_ros2::msg::RosTestMsg> publisher_access(publisher_);

    uint32_t count = 0;
    while (run_flag_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<uint32_t>(1000 / transport_frq_)));

      count++;
      MW_INFO("loop count : {} -------------------------", count);

      // publish ros event
      example_ros2::msg::RosTestMsg msg;
      msg.data = {1, 2, 3, 4};
      msg.num = count + 1000;

      MW_INFO("Publish new ros event, data:\n{}", example_ros2::msg::to_yaml(msg));
      publisher_access.Send(msg);
    }

    MW_INFO("Exit MainLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit MainLoop with exception, {}", e.what());
  }

  stop_sig_.set_value();
}

}  // namespace ibmw::examples::cpp::ros2_chn::normal_publisher_module
