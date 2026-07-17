// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "chn_subscriber_inline_module.h"
#include <memory>

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::ChnSubscriberInlineModule {
bool ChnSubscriberInlineModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();
  try {
    auto cfg_path = ctx_ptr_->GetConfigFilePath();
    if (!cfg_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(cfg_path));
      topic_name_ = cfg_node["topic"].as<std::string>();
    }

    subscriber_inline_ = ctx_ptr_->CreateSubscriber<ibmw::protocols::example::ExampleEventMsg>(
        topic_name_, [this](std::shared_ptr<const ibmw::protocols::example::ExampleEventMsg> msg) {
          if (ibmw::context::Running()) {
            const std::string msg_text = msg->msg();
            const int32_t msg_num = msg->num();
            MW_INFO("Received message: {} (num={})", msg_text, msg_num);
          }
        });

    MW_INFO("ChnSubscriberInlineModule initialized on topic '{}'.", topic_name_);
  } catch (const std::exception& e) {
    MW_ERROR("ChnSubscriberInlineModule init failed: {}", e.what());
    return false;
  }

  return true;
}

bool ChnSubscriberInlineModule::Run() {
  MW_INFO("ChnSubscriberInlineModule start succeeded.");
  return true;
}

void ChnSubscriberInlineModule::Stop() {
  ctx_ptr_->StopRunning();
  MW_INFO("ChnSubscriberInlineModule shutdown succeeded.");
}

}  // namespace ibmw::examples::cpp::context::ChnSubscriberInlineModule
