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
#if defined(IBMW_MULTI_DRIVER_DEMO_WITH_SHM)
  if (shm_drv_ != nullptr) {
    MW_INFO("Shm driver slot bound.");
  }
#else
  if (iox_drv_ != nullptr) {
    MW_INFO("Iceoryx driver slot bound.");
  }
#endif
  return true;
}

bool MultiDriverModule::Run() {
#if defined(IBMW_MULTI_DRIVER_DEMO_WITH_SHM)
  // The engine broadcasts driver slots after every module's Setup(), so this
  // is the first point where a config that uses the drivers sees them bound.
  if (dds_drv_ != nullptr) {
    MW_INFO("Run: DDS driver slot bound.");
  }
  if (shm_drv_ != nullptr) {
    MW_INFO("Run: Shm driver slot bound.");
  }
#endif
  MW_INFO("Run succeeded.");
  return true;
}

void MultiDriverModule::Stop() {
  MW_INFO("Stop succeeded.");
}

}  // namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module
