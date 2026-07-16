// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "chn_subscriber_on_exeutor_module/chn_subscriber_on_exeutor_module.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::ChnSubscriberOnExecutorModule {

bool ChnSubscriberOnExecutorModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();
  try {
    auto cfg_path = ctx_ptr_->GetConfigFilePath();
    if (!cfg_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(cfg_path));
      topic_name_ = cfg_node["topic"].as<std::string>();
      if (cfg_node["executor"]) {
        executor_name_ = cfg_node["executor"].as<std::string>();
      }
    }

    work_executor_ = ctx_ptr_->CreateExecutor(executor_name_);

    subscriber_on_executor_ = ctx_ptr_->CreateSubscriber<ibmw::protocols::example::ExampleEventMsg>(topic_name_, work_executor_, [this](std::shared_ptr<const ibmw::protocols::example::ExampleEventMsg> msg) {
      if (ibmw::context::Running()) {
        const std::string msg_text = msg->msg();
        const int32_t msg_num = msg->num();
        MW_INFO("Received message: {} (num={})", msg_text, msg_num);
      }
    });

    MW_INFO("ChnSubscriberOnExecutorModule initialized on topic '{}' with executor '{}'.", topic_name_, executor_name_);
  } catch (const std::exception& e) {
    MW_ERROR("ChnSubscriberOnExecutorModule init failed: {}", e.what());
    return false;
  }

  return true;
}

bool ChnSubscriberOnExecutorModule::Run() {
  MW_INFO("ChnSubscriberOnExecutorModule start succeeded.");
  return true;
}

void ChnSubscriberOnExecutorModule::Stop() {
  ctx_ptr_->StopRunning();
  MW_INFO("ChnSubscriberOnExecutorModule shutdown succeeded.");
}

}  // namespace ibmw::examples::cpp::context::ChnSubscriberOnExecutorModule
