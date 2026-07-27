# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

cmake_minimum_required(VERSION 3.24)

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_PACKAGE_NAME ${CUR_DIR})
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

project(${CUR_PACKAGE_NAME})

ibmw_find_target_package(ament_cmake REQUIRED FEATURE rosidl-schema)
if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  ibmw_find_target_package(rosidl_cmake REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_adapter REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_generator_c REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_generator_cpp REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_c REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_cpp REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_fastrtps_c REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_fastrtps_cpp REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_introspection_c REQUIRED FEATURE rosidl-schema)
  ibmw_find_target_package(rosidl_typesupport_introspection_cpp REQUIRED FEATURE rosidl-schema)
else()
  ibmw_find_target_package(rosidl_default_generators REQUIRED FEATURE rosidl-schema)
endif()
ibmw_find_target_package(builtin_interfaces REQUIRED FEATURE rosidl-schema)

# BUG of ros2
set(CUR_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS ON)
# cmake-format: off
rosidl_generate_interfaces(${CUR_PACKAGE_NAME}
  "msg/MessageHeader.msg"
  "msg/TouchSensorStateArray.msg"
  "msg/TouchSensorState.msg"
  "msg/JointStateArray.msg"
  "msg/JointState.msg"
  "msg/JointCommandArray.msg"
  "msg/JointCommand.msg"
  DEPENDENCIES  builtin_interfaces
)
# cmake-format: on
set(BUILD_SHARED_LIBS ${CUR_BUILD_SHARED_LIBS})

# Mirror the installed ROS interface layout for build-tree sidecar loading.
file(MAKE_DIRECTORY
  ${CMAKE_BINARY_DIR}/ament_cmake_index/share/${CUR_PACKAGE_NAME})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/msg
  DESTINATION ${CMAKE_BINARY_DIR}/ament_cmake_index/share/${CUR_PACKAGE_NAME})

if(NOT TARGET ${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_typesupport_cpp)
  add_library(${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_typesupport_cpp ALIAS ${CUR_PACKAGE_NAME}__rosidl_typesupport_cpp)
endif()

if(NOT TARGET ${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_generator_cpp)
  add_library(${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_generator_cpp ALIAS ${CUR_PACKAGE_NAME}__rosidl_generator_cpp)
endif()

if(NOT TARGET ${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp)
  add_library(${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp ALIAS ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp)
endif()

if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  ibmw_resolve_target_ros2_include_root(_ibmw_ros2_fastcdr_include fastcdr/Cdr.h)
  ibmw_resolve_target_ros2_library(_ibmw_ros2_fastcdr fastcdr)

  foreach(_ibmw_rosidl_target IN ITEMS
      ${CUR_PACKAGE_NAME}__rosidl_generator_cpp
      ${CUR_PACKAGE_NAME}__rosidl_typesupport_cpp
      ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp
      ${CUR_PACKAGE_NAME}__rosidl_typesupport_introspection_cpp)
    if(TARGET ${_ibmw_rosidl_target})
      set_target_properties(${_ibmw_rosidl_target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    endif()
  endforeach()

  foreach(_ibmw_rosidl_fastrtps_target IN ITEMS
      ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_c
      ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp)
    if(TARGET ${_ibmw_rosidl_fastrtps_target})
      target_include_directories(${_ibmw_rosidl_fastrtps_target}
        BEFORE PRIVATE
          "${_ibmw_ros2_fastcdr_include}")
    endif()
  endforeach()

  ibmw_cross_pin_rosidl_fastcdr("${_ibmw_ros2_fastcdr}"
    ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_c
    ${CUR_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp
    fastrtps
    FastRTPS::fastrtps
    rosidl_typesupport_fastrtps_c
    rosidl_typesupport_fastrtps_cpp
    rosidl_typesupport_fastrtps_c::rosidl_typesupport_fastrtps_c
    rosidl_typesupport_fastrtps_cpp::rosidl_typesupport_fastrtps_cpp)
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()

# Type support pkg
add_library(${CUR_TARGET_NAME}_ts SHARED ${CMAKE_CURRENT_SOURCE_DIR}/type_support_pkg_main.cc)
add_library(${CUR_TARGET_ALIAS_NAME}_ts ALIAS ${CUR_TARGET_NAME}_ts)
target_include_directories(${CUR_TARGET_NAME}_ts
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set(_ibmw_ros2_fastcdr fastcdr)
if(CMAKE_CROSSCOMPILING)
  ibmw_resolve_target_ros2_library(_ibmw_ros2_fastcdr fastcdr)
endif()
if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  set_target_properties(${CUR_TARGET_NAME}_ts PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
endif()
target_link_libraries(
  ${CUR_TARGET_NAME}_ts
  PRIVATE ibmw::api::ros2_type_support
          ${_ibmw_ros2_fastcdr}
          ${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_generator_cpp
          ${CUR_PACKAGE_NAME}::${CUR_PACKAGE_NAME}__rosidl_typesupport_cpp
          ${CUR_PACKAGE_NAME}__rosidl_typesupport_introspection_cpp)
set_target_properties(${CUR_TARGET_NAME}_ts PROPERTIES OUTPUT_NAME ${CUR_DIR}_ros2_ts)

if(IBMW_BUILD_TESTS)
  add_gtest_target(
    TEST_TARGET ${CUR_TARGET_NAME}_ts
    TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/ros2_schema_support_test.cc)
  set_tests_properties(${CUR_TARGET_NAME}_ts_test PROPERTIES
    ENVIRONMENT
      "AMENT_PREFIX_PATH=${CMAKE_BINARY_DIR}/ament_cmake_index:$ENV{AMENT_PREFIX_PATH}")
endif()

if(IBMW_INSTALL)
  install(TARGETS ${CUR_TARGET_NAME}_ts LIBRARY DESTINATION lib)
endif()
