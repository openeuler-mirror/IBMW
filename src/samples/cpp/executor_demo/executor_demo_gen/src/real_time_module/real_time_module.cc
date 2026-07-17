// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "real_time_module/real_time_module.h"

namespace executor_demo {

bool RealTimeModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("RealTimeModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool RealTimeModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("RealTimeModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void RealTimeModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("RealTimeModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace executor_demo
