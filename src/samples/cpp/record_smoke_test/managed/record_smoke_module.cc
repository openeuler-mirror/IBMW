// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "managed/record_smoke_module.h"

#include <memory>
#include <utility>

#include "generated/record_smoke_transport.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::record_smoke_test {

RecordSmokeModule::RecordSmokeModule(
    std::string module_name,
    std::string message_kind)
    : module_name_(std::move(module_name)),
      message_kind_(std::move(message_kind)) {}

bool RecordSmokeModule::Setup(ibmw::Runtime core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), !config_path.empty(),
        ibmw::common::util::kLogLevelError,
        "Record smoke module requires a configuration file.");
    const auto root = YAML::LoadFile(std::string(config_path));
    const auto node = root[module_name_] ? root[module_name_] : root;
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), node && node.IsMap(),
        ibmw::common::util::kLogLevelError,
        "Invalid configuration for '{}'.", module_name_);

    if (node["publish_topics"]) {
      publish_topics_ = node["publish_topics"].as<std::vector<std::string>>();
    }
    if (node["subscribe_topics"]) {
      subscribe_topics_ =
          node["subscribe_topics"].as<std::vector<std::string>>();
    }
    if (node["frequency_hz"]) {
      frequency_hz_ = node["frequency_hz"].as<double>();
    }
    if (node["clock_topic"]) {
      clock_topic_ = node["clock_topic"].as<std::string>();
    }
    if (node["clock_start_ns"]) {
      clock_start_ns_ = node["clock_start_ns"].as<uint64_t>();
    }
    if (node["clock_step_ns"]) {
      clock_step_ns_ = node["clock_step_ns"].as<uint64_t>();
    }
    if (node["clock_delay_ms"]) {
      clock_delay_ms_ = node["clock_delay_ms"].as<uint32_t>();
    }
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), frequency_hz_ > 0.0,
        ibmw::common::util::kLogLevelError,
        "frequency_hz must be greater than zero.");
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), !publish_topics_.empty() || !clock_topic_.empty(),
        ibmw::common::util::kLogLevelError,
        "Record smoke module must publish at least one topic.");

    for (const auto& topic : publish_topics_) {
      auto publisher = core_.GetTransport().GetPublisher(topic);
      MW_CHECK_LOG_THROW(
          core_.GetLogger(), publisher && RegisterPublisher(publisher),
          ibmw::common::util::kLogLevelError,
          "Register '{}' publisher for '{}' failed.", message_kind_, topic);
      publishers_.emplace_back(publisher);
    }
    for (const auto& topic : subscribe_topics_) {
      auto subscriber = core_.GetTransport().GetSubscriber(topic);
      MW_CHECK_LOG_THROW(
          core_.GetLogger(), subscriber && RegisterSubscriber(subscriber, topic),
          ibmw::common::util::kLogLevelError,
          "Register '{}' subscriber for '{}' failed.", message_kind_, topic);
      subscribers_.emplace_back(subscriber);
    }
    if (!clock_topic_.empty()) {
      clock_publisher_ = core_.GetTransport().GetPublisher(clock_topic_);
      MW_CHECK_LOG_THROW(
          core_.GetLogger(), clock_publisher_ &&
              generated::RegisterClockPublisher(clock_publisher_),
          ibmw::common::util::kLogLevelError,
          "Register rosgraph_msgs/msg/Clock publisher failed.");
    }
  } catch (const std::exception& error) {
    MW_LOG(
        core_.GetLogger(), ibmw::common::util::kLogLevelError,
        "{} setup failed: {}", module_name_, error.what());
    return false;
  }

  MW_LOG(
      core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
      "Record smoke module ready: name='{}', kind='{}', publishers={}, "
      "subscribers={}, clock='{}'.",
      module_name_, message_kind_, publish_topics_.size(),
      subscribe_topics_.size(), clock_topic_);
  return true;
}

bool RecordSmokeModule::RegisterPublisher(
    ibmw::transport::SenderRef publisher) {
  if (message_kind_ == "cdr") {
    return generated::RegisterCdrPublisher(publisher);
  }
  if (message_kind_ == "ros2") {
    return generated::RegisterRos2Publisher(publisher);
  }
  if (message_kind_ == "json") {
    return generated::RegisterJsonPublisher(publisher);
  }
  return false;
}

bool RecordSmokeModule::RegisterSubscriber(
    ibmw::transport::ReceiverRef subscriber,
    const std::string& topic) {
  const auto logger = core_.GetLogger();
  auto callback = [this, logger, topic](uint64_t sequence) {
    const auto count = received_count_.fetch_add(1) + 1;
    MW_LOG(
        logger, ibmw::common::util::kLogLevelInfo,
        "Record smoke received: kind='{}', topic='{}', count={}, sequence={}.",
        message_kind_, topic, count, sequence);
  };
  if (message_kind_ == "cdr") {
    return generated::RegisterCdrSubscriber(subscriber, std::move(callback));
  }
  if (message_kind_ == "ros2") {
    return generated::RegisterRos2Subscriber(subscriber, std::move(callback));
  }
  if (message_kind_ == "json") {
    return generated::RegisterJsonSubscriber(subscriber, std::move(callback));
  }
  return false;
}

bool RecordSmokeModule::Run() {
  if (running_.exchange(true)) {
    return true;
  }
  worker_ = std::thread([this] { PublishLoop(); });
  return true;
}

void RecordSmokeModule::Stop() {
  running_.store(false);
  if (worker_.joinable()) {
    worker_.join();
  }
  MW_LOG(
      core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
      "Record smoke module stopped: name='{}', received={}.", module_name_,
      received_count_.load());
}

void RecordSmokeModule::PublishLoop() {
  const auto period = std::chrono::duration<double>(1.0 / frequency_hz_);
  const auto started_at = std::chrono::steady_clock::now();
  uint64_t sequence = 0;
  uint64_t clock_sequence = 0;
  bool clock_active_logged = false;
  while (running_.load()) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    if (!clock_topic_.empty() &&
        elapsed >= std::chrono::milliseconds(clock_delay_ms_)) {
      PublishClock(clock_start_ns_ + clock_sequence++ * clock_step_ns_);
      if (!clock_active_logged) {
        MW_LOG(
            core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
            "Record smoke clock publisher active: topic='{}', start_ns={}.",
            clock_topic_, clock_start_ns_);
        clock_active_logged = true;
      }
    }
    for (auto publisher : publishers_) {
      Publish(publisher, sequence);
    }
    ++sequence;
    std::this_thread::sleep_for(period);
  }
}

void RecordSmokeModule::Publish(
    ibmw::transport::SenderRef publisher,
    uint64_t sequence) {
  if (message_kind_ == "cdr") {
    generated::PublishCdr(publisher, sequence);
    return;
  }
  if (message_kind_ == "ros2") {
    generated::PublishRos2(publisher, sequence);
    return;
  }
  generated::PublishJson(publisher, sequence);
}

void RecordSmokeModule::PublishClock(uint64_t timestamp_ns) {
  generated::PublishClock(clock_publisher_, timestamp_ns);
}

}  // namespace ibmw::examples::cpp::record_smoke_test
