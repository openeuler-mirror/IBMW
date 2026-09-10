// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "cdr_publisher_module/cdr_publisher_module.h"

#include <chrono>

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::record_demo::cdr_publisher_module {

bool CdrPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    if (!config_path.empty()) {
      const auto root = YAML::LoadFile(std::string(config_path));
      const auto node = root["CdrPublisherModule"] ?
                            root["CdrPublisherModule"] : root;
      if (node["topic"]) {
        topic_name_ = node["topic"].as<std::string>();
      }
      if (node["transport_frq"]) {
        transport_frq_ = node["transport_frq"].as<double>();
      }
    }
    MW_CHECK_LOG_THROW(core_.GetLogger(), transport_frq_ > 0.0,
                       ibmw::common::util::kLogLevelError,
                       "transport_frq must be greater than zero.");

    auto publisher = core_.GetTransport().GetPublisher(topic_name_);
    MW_CHECK_LOG_THROW(core_.GetLogger(), publisher,
                       ibmw::common::util::kLogLevelError,
                       "Get publisher for topic '{}' failed.", topic_name_);
    MW_CHECK_LOG_THROW(
        core_.GetLogger(),
        publisher_.Initialize(publisher),
        ibmw::common::util::kLogLevelError,
        "Register CDR Header TypeSupport failed.");
  } catch (const std::exception& error) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "CdrPublisherModule setup failed: {}", error.what());
    return false;
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "CdrPublisherModule ready: topic='{}', serialization='cdr'.", topic_name_);
  return true;
}

bool CdrPublisherModule::Run() {
  if (running_.exchange(true)) {
    return true;
  }
  worker_ = std::thread([this] { PublishLoop(); });
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "CdrPublisherModule running.");
  return true;
}

void CdrPublisherModule::Stop() {
  running_.store(false);
  if (worker_.joinable()) {
    worker_.join();
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "CdrPublisherModule stopped.");
}

void CdrPublisherModule::PublishLoop() {
  const auto period = std::chrono::duration<double>(1.0 / transport_frq_);
  while (running_.load()) {
    const auto sequence = sequence_++;
    publisher_.Publish(sequence);
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
        "Published CDR Header: topic='{}', sequence={}", topic_name_,
        sequence);
    std::this_thread::sleep_for(period);
  }
}

}  // namespace ibmw::examples::cpp::record_demo::cdr_publisher_module
