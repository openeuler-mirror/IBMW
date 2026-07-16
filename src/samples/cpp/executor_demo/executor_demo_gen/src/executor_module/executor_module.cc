// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "executor_module/executor_module.h"

namespace executor_demo {

bool ExecutorModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool ExecutorModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void ExecutorModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace executor_demo
