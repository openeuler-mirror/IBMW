// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "executor_co_loop_module/executor_co_loop_module.h"

namespace executor_demo {

bool ExecutorCoLoopModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoLoopModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool ExecutorCoLoopModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoLoopModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void ExecutorCoLoopModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoLoopModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace executor_demo
