// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "timer_module/timer_module.h"

namespace executor_demo {

bool TimerModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("TimerModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool TimerModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("TimerModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void TimerModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("TimerModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace executor_demo
