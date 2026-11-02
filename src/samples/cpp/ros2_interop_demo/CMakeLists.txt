# Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Ibmw is licensed under Mulan PSL v2.

cmake_minimum_required(VERSION 3.24)

# 检查ROS2接口库是否可用
if(NOT TARGET ibmw::extensions::ros2_types)
  message(WARNING "ibmw_module_ros2_interface not found, skipping ros2_interop_demo")
  return()
endif()

message(STATUS "Building ros2_interop_demo")

# ========== Publisher Module ==========
add_library(ros2_interop_publisher_module STATIC
  managed/publisher_module/publisher_module.cc
)

target_include_directories(ros2_interop_publisher_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/managed
)

target_link_libraries(ros2_interop_publisher_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
)

# Force link the entire impl library to ensure static registrars are initialized
if(APPLE)
  target_link_libraries(ros2_interop_publisher_module
    PUBLIC
      -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(ros2_interop_publisher_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(ros2_interop_publisher_module
    PUBLIC
      ibmw::extensions::ros2_types_impl
  )
endif()

# ========== Subscriber Module ==========
add_library(ros2_interop_subscriber_module STATIC
  managed/subscriber_module/subscriber_module.cc
)

target_include_directories(ros2_interop_subscriber_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/managed
)

target_link_libraries(ros2_interop_subscriber_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
)

# Force link the entire impl library to ensure static registrars are initialized
if(APPLE)
  target_link_libraries(ros2_interop_subscriber_module
    PUBLIC
      -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(ros2_interop_subscriber_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(ros2_interop_subscriber_module
    PUBLIC
      ibmw::extensions::ros2_types_impl
  )
endif()

# ========== App Mode Executable ==========
add_executable(ros2_interop_app
  standalone/ros2_interop_app/main.cc
)

target_link_libraries(ros2_interop_app
  PRIVATE
    ros2_interop_publisher_module
    ros2_interop_subscriber_module
    ibmw::core::engine
)

# ========== Publisher Only App ==========
add_executable(ros2_pub_only_app
  standalone/ros2_pub_only_app/main.cc
)

target_link_libraries(ros2_pub_only_app
  PRIVATE
    ros2_interop_publisher_module
    ibmw::core::engine
)

# ========== Subscriber Only App ==========
add_executable(ros2_sub_only_app
  standalone/ros2_sub_only_app/main.cc
)

target_link_libraries(ros2_sub_only_app
  PRIVATE
    ros2_interop_subscriber_module
    ibmw::core::engine
)

# ========== 安装 ==========
install(TARGETS ros2_interop_app ros2_pub_only_app ros2_sub_only_app
  RUNTIME DESTINATION bin
)

install(DIRECTORY config/
  DESTINATION share/ros2_interop_demo
  USE_SOURCE_PERMISSIONS
  PATTERN ".*" EXCLUDE
)

message(STATUS "ros2_interop_demo configured successfully")
