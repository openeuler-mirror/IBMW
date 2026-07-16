// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "logger_module/logger_module.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::logger::logger_module {
bool LoggerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  executor_ = core_.GetExecutor().GetExecutor("work_executor");
  MW_CHECK_LOG_THROW(core_.GetLogger(), executor_, ibmw::common::util::kLogLevelError, "Get executor 'work_thread_pool' failed.");
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "Setup succeeded.");
  return true;
}
bool LoggerModule::Run() {
  run_flag_ = true;
  executor_.Dispatch([this]() {
    auto GetLogger = [this]() { return core_.GetLogger(); };
    std::string s = "abc";
    int n = 0;
    while (run_flag_.load()) {
      ++n;
      MW_TRACE("Test trace log, s = {}, n = {}.", s, n);
      MW_DEBUG("Test debug log, s = {}, n = {}.", s, n);
      MW_INFO("Test info log, s = {}, n = {}.", s, n);
      MW_WARN("Test warn log, s = {}, n = {}.", s, n);
      MW_ERROR("Test error log, s = {}, n = {}.", s, n);
      MW_FATAL("Test fatal log, s = {}, n = {}.", s, n);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    stop_sig_.set_value();
  });
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "Run succeeded.");
  return true;
}
void LoggerModule::Stop() {
  if (run_flag_) {
    run_flag_ = false;
    stop_sig_.get_future().wait();
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "Stop succeeded.");
}
}
