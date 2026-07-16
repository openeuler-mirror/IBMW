// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "multi_driver_module/multi_driver_module.h"

namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module {

bool MultiDriverModule::Setup(ibmw::Runtime core) {
  core_ = core;
  MW_INFO("Setup succeeded.");
  if (dds_drv_ != nullptr) {
    MW_INFO("DDS driver slot bound.");
  }
  if (iox_drv_ != nullptr) {
    MW_INFO("Iceoryx driver slot bound.");
  }
  return true;
}

bool MultiDriverModule::Run() {
  MW_INFO("Run succeeded.");
  return true;
}

void MultiDriverModule::Stop() {
  MW_INFO("Stop succeeded.");
}

}  // namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module
