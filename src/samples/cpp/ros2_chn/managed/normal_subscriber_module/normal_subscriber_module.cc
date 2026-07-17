// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "normal_subscriber_module/normal_subscriber_module.h"
#include "internal/typesupport/ros2/ros2_transport.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::ros2_chn::normal_subscriber_module {

bool NormalSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Read cfg
    auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      topic_name_ = cfg_node["topic"].as<std::string>();
    }

    // Subscribe
    subscriber_ = core_.GetTransport().GetSubscriber(topic_name_);
    MW_REQUIRE(subscriber_, "Get subscriber for topic '{}' failed.", topic_name_);

    bool ret = ibmw::transport::OnMessage<example_ros2::msg::RosTestMsg>(
        subscriber_,
        std::bind(&NormalSubscriberModule::EventHandle, this, std::placeholders::_1));
    MW_REQUIRE(ret, "Subscribe failed.");

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool NormalSubscriberModule::Run() { return true; }

void NormalSubscriberModule::Stop() {}

void NormalSubscriberModule::EventHandle(const std::shared_ptr<const example_ros2::msg::RosTestMsg>& data) {
  MW_INFO("Receive new ros event, data:\n{}", example_ros2::msg::to_yaml(*data));
}

}  // namespace ibmw::examples::cpp::ros2_chn::normal_subscriber_module
