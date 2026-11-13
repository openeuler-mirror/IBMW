// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "my_module/my_module.h"

namespace my_project {

bool MyModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MyModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool MyModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MyModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void MyModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("MyModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace my_project
