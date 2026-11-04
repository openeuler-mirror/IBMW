# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Set namespace
set_namespace()

# module
add_subdirectory(managed/normal_publisher_module)
add_subdirectory(managed/normal_subscriber_module)
add_subdirectory(managed/benchmark_publisher_module)
add_subdirectory(managed/benchmark_subscriber_module)

# pkg
add_subdirectory(package/ros2_chn_pkg)
add_subdirectory(package/ros2_chn_pub_pkg)
add_subdirectory(package/ros2_chn_sub_pkg)

# app
add_subdirectory(standalone/normal_ros2_chn_publisher_app)
add_subdirectory(standalone/normal_ros2_chn_subscriber_app)
add_subdirectory(standalone/native_ros2_benchmark_app)

# install
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "Unsupport os")
endif()

# build all
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})
add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS ibmw::core::main
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::native_ros2_benchmark_app
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ros2_chn_pkg
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ros2_chn_pub_pkg
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ros2_chn_sub_pkg)
