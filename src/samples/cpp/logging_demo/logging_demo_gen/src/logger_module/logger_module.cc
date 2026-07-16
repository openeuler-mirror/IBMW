// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "logger_module/logger_module.h"

#include <chrono>
#include <thread>

namespace logging_demo {

bool LoggerModule::Setup(ibmw::Runtime core) {
  core_ = core;

  executor_ = core_.GetExecutor().GetExecutor("work_executor");
  MW_CHECK_LOG_THROW(
      core_.GetLogger(), executor_,
      ibmw::common::util::kLogLevelError,
      "Get executor 'work_executor' failed.");

  MW_INFO("LoggerModule::Setup succeeded.");
  return true;
}

bool LoggerModule::Run() {
  run_flag_ = true;

  executor_.Dispatch([this]() {
    auto GetLogger = [this]() { return core_.GetLogger(); };

    uint64_t n = 0;
    while (run_flag_) {
      std::string s = "log_msg_" + std::to_string(n);

      MW_TRACE("Test trace log, s = {}, n = {}.", s, n);
      MW_DEBUG("Test debug log, s = {}, n = {}.", s, n);
      MW_INFO("Test info log, s = {}, n = {}.", s, n);
      MW_WARN("Test warn log, s = {}, n = {}.", s, n);
      MW_ERROR("Test error log, s = {}, n = {}.", s, n);
      MW_FATAL("Test fatal log, s = {}, n = {}.", s, n);

      ++n;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    stop_promise_.set_value();
  });

  MW_INFO("LoggerModule::Run succeeded.");
  return true;
}

void LoggerModule::Stop() {
  run_flag_ = false;
  auto f = stop_promise_.get_future();
  f.wait();
  MW_INFO("LoggerModule::Stop succeeded.");
}

}  // namespace logging_demo
