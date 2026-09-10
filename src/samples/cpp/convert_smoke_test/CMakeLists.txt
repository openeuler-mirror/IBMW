# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Independent smoke coverage for the IBMW bag convert command.  The test owns
# its input bag and storage configuration; it does not consume bag_convert_demo.
if(NOT IBMW_BUILD_RECORD_PLAYBACK_EXTENSION OR NOT IBMW_BUILD_WITH_ROS2)
  message(STATUS "Skip convert_smoke_test: record/playback and ROS2 are required")
  return()
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(STATUS "Skip convert_smoke_test: Linux is required")
  return()
endif()

add_custom_target(
  ibmw_samples_cpp_convert_smoke_test_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/bags
          ${CMAKE_BINARY_DIR}/convert_smoke_bags
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin/cfg
          ${CMAKE_BINARY_DIR}/convert_smoke_cfg
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/validate_convert_smoke.sh
          ${CMAKE_BINARY_DIR}/validate_convert_smoke.sh
  COMMAND ${CMAKE_COMMAND} -E touch
          ${CMAKE_BINARY_DIR}/convert_smoke_required
  COMMAND chmod +x ${CMAKE_BINARY_DIR}/validate_convert_smoke.sh
  DEPENDS ibmw::core::main
          ibmw_extensions_record_playback_ext)
