// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "parameter_module/parameter_module.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::parameter::parameter_module {

bool ParameterModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    work_executor_ = core_.GetExecutor().GetExecutor("work_thread_pool");
    MW_REQUIRE(
        work_executor_ && work_executor_.SupportsTimedDispatch(),
        "Get executor 'work_thread_pool' failed.");

    parameter_handle_ = core_.GetParams();
    MW_REQUIRE(parameter_handle_, "Get parameter failed.");

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool ParameterModule::Run() {
  try {
    work_executor_.Dispatch(std::bind(&ParameterModule::SetParameterLoop, this));
    work_executor_.Dispatch(std::bind(&ParameterModule::GetParameterLoop, this));
  } catch (const std::exception& e) {
    MW_ERROR("Start failed, {}", e.what());
    return false;
  }

  MW_INFO("Run succeeded.");
  return true;
}

void ParameterModule::Stop() {
  try {
    run_flag_ = false;
    set_loop_stop_sig_.get_future().wait();
    get_loop_stop_sig_.get_future().wait();
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }

  MW_INFO("Stop succeeded.");
}

void ParameterModule::SetParameterLoop() {
  try {
    MW_INFO("Start SetParameterLoop.");

    uint32_t count = 0;
    while (run_flag_) {
      count++;
      MW_INFO("SetParameterLoop count : {} -------------------------", count);

      std::string key = "key-" + std::to_string(count);
      std::string val = "val-" + std::to_string(count);
      parameter_handle_.SetParameter(key, val);
      MW_INFO("Set parameter, key: '{}', val: '{}'", key, val);

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    MW_INFO("Exit SetParameterLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit SetParameterLoop with exception, {}", e.what());
  }

  set_loop_stop_sig_.set_value();
}

void ParameterModule::GetParameterLoop() {
  try {
    MW_INFO("Start GetParameterLoop.");

    uint32_t count = 0;
    while (run_flag_) {
      count++;
      MW_INFO("GetParameterLoop count : {} -------------------------", count);

      std::string key = "key-" + std::to_string(count);
      auto val = parameter_handle_.GetParameter(key);
      MW_INFO("Get parameter, key: '{}', val: '{}'", key, val);

      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    MW_INFO("Exit GetParameterLoop.");
  } catch (const std::exception& e) {
    MW_ERROR("Exit GetParameterLoop with exception, {}", e.what());
  }

  get_loop_stop_sig_.set_value();
}

}  // namespace ibmw::examples::cpp::parameter::parameter_module
