# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

get_filename_component(CUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

add_library(${CUR_TARGET_NAME} SHARED pkg_main.cc)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE ibmw::api::pkg
          ${CUR_SUPERIOR_NAMESPACE}::cdr_publisher_module
          ${CUR_SUPERIOR_NAMESPACE}::ros2_publisher_module)

set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_DIR})
