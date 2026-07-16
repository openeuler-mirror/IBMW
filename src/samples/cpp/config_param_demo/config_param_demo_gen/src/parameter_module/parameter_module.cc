// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "parameter_module/parameter_module.h"

namespace config_param_demo {

bool ParameterModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ParameterModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool ParameterModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ParameterModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void ParameterModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ParameterModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace config_param_demo
