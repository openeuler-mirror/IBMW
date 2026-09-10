// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::playback_demo::subscriber_module {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;
  try {
    const auto config_path = core_.GetConfigurator().GetConfigFilePath();
    if (!config_path.empty()) {
      const auto root = YAML::LoadFile(std::string(config_path));
      const auto node = root["PlaybackSubscriberModule"]
                            ? root["PlaybackSubscriberModule"]
                            : root;
      if (node["topic"]) {
        topic_name_ = node["topic"].as<std::string>();
      }
    }

    subscriber_ = core_.GetTransport().GetSubscriber(topic_name_);
    MW_CHECK_LOG_THROW(core_.GetLogger(), subscriber_,
                       ibmw::common::util::kLogLevelError,
                       "Get subscriber for topic '{}' failed.", topic_name_);

    const auto logger = core_.GetLogger();
    MW_CHECK_LOG_THROW(
        core_.GetLogger(),
        subscriber_adapter_.Subscribe(
            subscriber_,
            [this, logger](const generated::PlaybackMessage& message) {
              const auto count = received_count_.fetch_add(1) + 1;
              MW_LOG(
                  logger, ibmw::common::util::kLogLevelInfo,
                  "Received playback message #{}: sequence={}, frame_id='{}', "
                  "timestamp={}.",
                  count, message.sequence, message.frame_id,
                  message.timestamp);
            }),
        ibmw::common::util::kLogLevelError,
        "Subscribe to playback topic '{}' failed.", topic_name_);
  } catch (const std::exception& error) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PlaybackSubscriberModule setup failed: {}", error.what());
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PlaybackSubscriberModule ready on topic '{}'.", topic_name_);
  return true;
}

bool SubscriberModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PlaybackSubscriberModule running; start ibmw bag play in another terminal.");
  return true;
}

void SubscriberModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PlaybackSubscriberModule stopped after receiving {} message(s).",
         received_count_.load());
}

}  // namespace ibmw::examples::cpp::playback_demo::subscriber_module
