// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "my_module/my_module.h"

namespace my_project {

bool MyModule::Setup(ibmw::Runtime core) {
  core_ = core;
  MW_INFO("MyModule::Setup succeeded.");
  return true;
}

bool MyModule::Run() {
  MW_INFO("MyModule::Run succeeded.");
  return true;
}

void MyModule::Stop() {
  MW_INFO("MyModule::Stop succeeded.");
}

}  // namespace my_project
