// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "multi_driver_module/multi_driver_module.h"

namespace ibmw::examples::cpp::multi_driver_demo {

bool MultiDriverModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MultiDriverModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool MultiDriverModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MultiDriverModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void MultiDriverModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MultiDriverModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace ibmw::examples::cpp::multi_driver_demo
