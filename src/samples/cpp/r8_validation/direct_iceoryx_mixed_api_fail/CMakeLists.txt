# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DELIBERATE BUILD FAILURE: this target trips a static_assert defined in
# iceoryx_transport_driver.h (IbmwR82SlotMixCheck<>). Only built when
# IBMW_BUILD_R8_FAIL_SAMPLES=ON; the parent CMakeLists.txt already gates
# the add_subdirectory() call, so this file is reached only in that mode.

add_executable(r8_validation_direct_iceoryx_mixed_api_fail
  standalone/main.cc)

target_include_directories(r8_validation_direct_iceoryx_mixed_api_fail PRIVATE
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)

target_link_libraries(r8_validation_direct_iceoryx_mixed_api_fail PRIVATE
  ibmw::core::engine
  ibmw_extensions_iceoryx_ext
  iceoryx_posh::iceoryx_posh)
