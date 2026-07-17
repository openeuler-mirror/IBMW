// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "executor_co_module/executor_co_module.h"

namespace executor_demo {

bool ExecutorCoModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool ExecutorCoModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void ExecutorCoModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("ExecutorCoModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace executor_demo
