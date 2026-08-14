# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Pre-recorded IBMW/rosbag2 conversion demonstration.

if(NOT IBMW_BUILD_RECORD_PLAYBACK_EXTENSION OR NOT IBMW_BUILD_WITH_ROS2)
  message(STATUS "Skip bag_convert_demo: record/playback or ROS2 is disabled")
  return()
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(FATAL_ERROR "bag_convert_demo currently supports Linux only")
endif()

get_filename_component(CUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/bags/ibmw_mcap
          ${CMAKE_BINARY_DIR}/bag_convert_demo_input/ibmw_mcap
  DEPENDS ibmw::core::main
          ibmw_extensions_record_playback_ext)
