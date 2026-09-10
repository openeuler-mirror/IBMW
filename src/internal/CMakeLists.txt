# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Internal helpers are build-only. No target or header in this directory is
# installed or exported as public API.
file(GLOB ros2_action_dds_head_files
     ${CMAKE_CURRENT_SOURCE_DIR}/ros2_action_dds/*.h)

add_library(ibmw_internal_ros2_action_dds INTERFACE)
add_library(ibmw::internal::ros2_action_dds ALIAS
            ibmw_internal_ros2_action_dds)
target_include_directories(
  ibmw_internal_ros2_action_dds
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_sources(
  ibmw_internal_ros2_action_dds
  INTERFACE ${ros2_action_dds_head_files})
