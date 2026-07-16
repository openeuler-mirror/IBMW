// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "hello_world_module/hello_world_module.h"

namespace ibmw::examples::cpp::helloworld {

bool HelloWorldModule::Setup(ibmw::Runtime core) {
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("HelloWorldModule::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

bool HelloWorldModule::Run() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("HelloWorldModule::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}

void HelloWorldModule::Stop() {
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("HelloWorldModule::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}

}  // namespace ibmw::examples::cpp::helloworld
