// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Entry of the EXT tests built as a shared object for the OHOS test HAP
// (IBMW_BUILD_SHM_EXT_HAP_CHECK): a native child process of the HAP calls it
// in place of gtest's main, because HAP processes cannot exec files.
#include <dlfcn.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <typeinfo>

extern "C" __attribute__((visibility("default"))) int ShmExtHapRunTests(int argc, char** argv) {
  // The typeinfo copies this library is bound to: libc++_shared.so's, see
  // shm_ext_hap.cc.
  auto owner = [](const void* addr) {
    Dl_info info{};
    return dladdr(addr, &info) != 0 && info.dli_fname != nullptr ? info.dli_fname : "unresolved";
  };
  std::printf("HAP_ABI test typeid(std::exception)=%s typeid(std::runtime_error)=%s\n", owner(&typeid(std::exception)),
              owner(&typeid(std::runtime_error)));
  std::fflush(stdout);
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
