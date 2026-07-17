// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "logger/logger_module.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::context::logger_module {

bool LoggerModule::Setup(ibmw::Runtime core) {
  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  // Get executor handle
  executor_ = ctx_ptr_->CreateExecutor("work_executor");

  return true;
}

bool LoggerModule::Run() {
  executor_.Dispatch([this]() {
    ctx_ptr_->LetMe();

    std::string s = "abc";
    int n = 0;

    while (ibmw::context::Running()) {
      ++n;

      ibmw::context::Log().Trace("Test trace log, s = {}, n = {}.", s, n);
      ibmw::context::Log().Debug("Test debug log, s = {}, n = {}.", s, n);
      ibmw::context::Log().Info("Test info log, s = {}, n = {}.", s, n);
      ibmw::context::Log().Warn("Test warn log, s = {}, n = {}.", s, n);
      ibmw::context::Log().Error("Test error log, s = {}, n = {}.", s, n);
      ibmw::context::Log().Fatal("Test fatal log, s = {}, n = {}.", s, n);

      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  ibmw::context::Log().Info("Run succeeded.");

  return true;
}

void LoggerModule::Stop() {
  ctx_ptr_->StopRunning();
  ibmw::context::Log().Info("Stop succeeded.");
}

}  // namespace ibmw::examples::cpp::context::logger_module
