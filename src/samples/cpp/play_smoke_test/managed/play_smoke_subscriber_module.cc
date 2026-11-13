// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "managed/play_smoke_subscriber_module.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#include <unistd.h>

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::play_smoke_test {

PlaySmokeSubscriberModule::PlaySmokeSubscriberModule(
    std::string name, std::string serialization)
    : name_(std::move(name)), serialization_(std::move(serialization)) {}

bool PlaySmokeSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    const auto root = YAML::LoadFile(std::string(config_path));
    const auto node = root[name_] ? root[name_] : root;
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), node && node["topics"],
        ibmw::common::util::kLogLevelError,
        "{} requires a topics sequence.", name_);
    topics_ = node["topics"].as<std::vector<std::string>>();
    const char* output_environment = std::getenv("PLAY_SMOKE_OUTPUT");
    if (output_environment != nullptr) {
      output_path_ = output_environment;
    } else if (node["output_file"]) {
      output_path_ = node["output_file"].as<std::string>();
    } else {
      // Per process, so parallel runs and other users do not share the file.
      output_path_ = std::filesystem::temp_directory_path() /
                     ("ibmw-play-smoke-received-" + std::to_string(::getpid()) + ".log");
    }
    output_.open(output_path_, std::ios::out | std::ios::trunc);
    MW_CHECK_LOG_THROW(
        core_.GetLogger(), output_.is_open(),
        ibmw::common::util::kLogLevelError,
        "Cannot open play smoke output '{}'.", output_path_.string());

    for (const auto& topic : topics_) {
      auto subscriber = core_.GetTransport().GetSubscriber(topic);
      MW_CHECK_LOG_THROW(
          core_.GetLogger(), subscriber && Subscribe(subscriber, topic),
          ibmw::common::util::kLogLevelError,
          "Subscribe '{}' using '{}' failed.", topic, serialization_);
      subscribers_.emplace_back(subscriber);
    }
  } catch (const std::exception& error) {
    MW_LOG(
        core_.GetLogger(), ibmw::common::util::kLogLevelError,
        "{} setup failed: {}", name_, error.what());
    return false;
  }
  MW_LOG(
      core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
      "Play smoke subscriber ready: name='{}', serialization='{}', topics={}, output='{}'.",
      name_, serialization_, topics_.size(), output_path_.string());
  return true;
}

bool PlaySmokeSubscriberModule::Subscribe(
    ibmw::transport::ReceiverRef subscriber, const std::string& topic) {
  auto callback = [this, topic](generated::ReceivedMessage message) {
    Receive(topic, std::move(message));
  };
  if (serialization_ == "cdr") {
    return generated::RegisterCdrSubscriber(
        subscriber, std::move(callback));
  }
  if (serialization_ == "json") {
    return generated::RegisterJsonSubscriber(
        subscriber, std::move(callback));
  }
  if (serialization_ == "ros2") {
    return generated::RegisterRos2Subscriber(
        subscriber, std::move(callback));
  }
  return false;
}

void PlaySmokeSubscriberModule::Receive(
    const std::string& topic, generated::ReceivedMessage message) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto count = received_.fetch_add(1) + 1;
  {
    std::lock_guard lock(output_mutex_);
    output_ << topic << '|' << message.sequence << '|' << message.payload
            << '|' << message.serialization << '|' << message.driver << '|'
            << timestamp << '\n';
    output_.flush();
  }
  MW_LOG(
      core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
      "Play smoke received: topic='{}', sequence={}, payload='{}', "
      "serialization='{}', driver='{}', count={}.",
      topic, message.sequence, message.payload, message.serialization,
      message.driver, count);
}

bool PlaySmokeSubscriberModule::Run() { return true; }

void PlaySmokeSubscriberModule::Stop() {
  std::lock_guard lock(output_mutex_);
  if (output_.is_open()) {
    output_.flush();
    output_.close();
  }
}

}  // namespace ibmw::examples::cpp::play_smoke_test
