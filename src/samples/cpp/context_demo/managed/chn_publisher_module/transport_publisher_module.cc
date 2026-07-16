// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <chrono>

#include "chn_publisher_module/transport_publisher_module.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::transport_publisher_module {

bool ChnPublisherModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  try {
    auto cfg_path = ctx_ptr_->GetConfigFilePath();
    if (!cfg_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(cfg_path));
      topic_name_ = cfg_node["topic"].as<std::string>();
      transport_frq_ = cfg_node["transport_frq"].as<double>();
      if (cfg_node["executor"]) {
        executor_name_ = cfg_node["executor"].as<std::string>();
      }
    }

    work_executor_ = ctx_ptr_->CreateExecutor(executor_name_);
    publisher_ = ctx_ptr_->CreatePublisher<ibmw::protocols::example::ExampleEventMsg>(topic_name_);
    MW_INFO("Transport sender initialized on topic '{}' with frequency {} Hz on executor '{}'.", topic_name_, transport_frq_, executor_name_);
  } catch (const std::exception& e) {
    MW_ERROR("TransportPublisherModule init failed: {}", e.what());
    return false;
  }

  return true;
}

bool ChnPublisherModule::Run() {
  work_executor_.Dispatch([this]() {
    ctx_ptr_->LetMe();
    RunPublishLoopTask();
  });

  MW_INFO("TransportPublisherModule start.");
  return true;
}

void ChnPublisherModule::Stop() {
  ctx_ptr_->StopRunning();
  MW_INFO("TransportPublisherModule shutdown.");
}

void ChnPublisherModule::RunPublishLoopTask() {
  using namespace std::chrono_literals;

  const auto interval_ms_double = 1000.0 / transport_frq_;

  const auto interval = std::chrono::milliseconds(static_cast<uint32_t>(interval_ms_double));

  uint32_t count = 0;

  while (ibmw::context::Running()) {
    if (interval.count() > 0) {
      std::this_thread::sleep_for(interval);
    }
    ++count;
    ibmw::protocols::example::ExampleEventMsg message;
    message.msg("Context transport message #" + std::to_string(count));
    message.num(static_cast<int32_t>(count));
    publisher_.Send(message);

    MW_INFO("Published message: {} (num={})", message.msg(), message.num());
  }

  MW_INFO("TransportPublisherModule publish loop exited.");
}

}  // namespace ibmw::examples::cpp::context::transport_publisher_module
