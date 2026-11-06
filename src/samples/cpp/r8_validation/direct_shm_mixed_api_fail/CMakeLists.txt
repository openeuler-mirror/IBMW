# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DELIBERATE BUILD FAILURE: this target trips a static_assert defined in
# shm_transport_driver.h (IbmwR82SlotMixCheck<>), the shm_ext copy of the
# iceoryx_ext trip wire. Only built when IBMW_BUILD_R8_FAIL_SAMPLES=ON; the
# parent CMakeLists.txt already gates the add_subdirectory() call, so this
# file is reached only in that mode.

add_executable(r8_validation_direct_shm_mixed_api_fail
  standalone/main.cc)

target_link_libraries(r8_validation_direct_shm_mixed_api_fail PRIVATE
  ibmw::core::engine
  ibmw_extensions_shm_ext)
