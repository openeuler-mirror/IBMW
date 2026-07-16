# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_SERVER_TARGET_NAME ibmw_fibonacci_action_dds_server)
set(CUR_SERVER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_server/fibonacci_action_server.cpp)
set(CUR_SERVER_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_fibonacci_action_dds_server)

set(CUR_GENERIC_SERVER_TARGET_NAME ibmw_action_dds_generic_fibonacci_server)
set(CUR_GENERIC_SERVER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_server/generic_fibonacci_action_server.cpp)
set(CUR_GENERIC_SERVER_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_action_dds_generic_fibonacci_server)

set(CUR_COUNTDOWN_SERVER_TARGET_NAME ibmw_action_dds_countdown_fibonacci_wire_server)
set(CUR_COUNTDOWN_SERVER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_server/countdown_fibonacci_wire_action_server.cpp)
set(CUR_COUNTDOWN_SERVER_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_action_dds_countdown_fibonacci_wire_server)

set(CUR_GRIPPER_SERVER_TARGET_NAME ibmw_action_dds_generic_gripper_command_server)
set(CUR_GRIPPER_SERVER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_server/generic_gripper_command_action_server.cpp)
set(CUR_GRIPPER_SERVER_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_action_dds_generic_gripper_command_server)

set(CUR_CLIENT_TARGET_NAME ibmw_fibonacci_action_ros2_client)
set(CUR_CLIENT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_client/fibonacci_action_client.cpp)
set(CUR_CLIENT_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_fibonacci_action_ros2_client)

set(CUR_GRIPPER_CLIENT_TARGET_NAME ibmw_gripper_command_action_ros2_client)
set(CUR_GRIPPER_CLIENT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ros2_client/gripper_command_action_client.cpp)
set(CUR_GRIPPER_CLIENT_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::ibmw_gripper_command_action_ros2_client)

ibmw_find_target_package(action_msgs QUIET FEATURE ros2-action-proof)
ibmw_find_target_package(builtin_interfaces QUIET FEATURE ros2-action-proof)
ibmw_find_target_package(control_msgs QUIET FEATURE ros2-action-proof)
ibmw_find_target_package(example_interfaces QUIET FEATURE ros2-action-proof)
ibmw_find_target_package(unique_identifier_msgs QUIET FEATURE ros2-action-proof)
if(IBMW_BUILD_WITH_ROS2)
  ibmw_find_target_package(rclcpp QUIET FEATURE ros2-action-proof-client)
  ibmw_find_target_package(rclcpp_action QUIET FEATURE ros2-action-proof-client)
endif()

set(CUR_DDS_ROS2_HEADERS
  action_msgs::action_msgs__rosidl_generator_cpp
  builtin_interfaces::builtin_interfaces__rosidl_generator_cpp
  control_msgs::control_msgs__rosidl_generator_cpp
  example_interfaces::example_interfaces__rosidl_generator_cpp
  unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp)

set(CUR_DDS_ROS2_LIBS)
if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  ibmw_resolve_target_ros2_include_root(CUR_ROS2_INCLUDE_ROOT rmw/rmw/types.h)
  set(CUR_ROS2_RMW_INCLUDE_DIR ${CUR_ROS2_INCLUDE_ROOT}/rmw)
  foreach(_ibmw_action_ros2_lib IN ITEMS
      example_interfaces__rosidl_typesupport_fastrtps_cpp
      control_msgs__rosidl_typesupport_fastrtps_cpp
      action_msgs__rosidl_typesupport_fastrtps_cpp
      builtin_interfaces__rosidl_typesupport_fastrtps_cpp
      unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp
      rosidl_typesupport_fastrtps_cpp
      rosidl_runtime_c
      rcutils)
    ibmw_resolve_target_ros2_library(_ibmw_action_ros2_lib_path ${_ibmw_action_ros2_lib})
    list(APPEND CUR_DDS_ROS2_LIBS ${_ibmw_action_ros2_lib_path})
  endforeach()
else()
  list(APPEND CUR_DDS_ROS2_LIBS
    action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp
    builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp
    control_msgs::control_msgs__rosidl_typesupport_fastrtps_cpp
    example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
    unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp)
endif()

set(CUR_NATIVE_ROS2_TYPES
  ${CUR_DDS_ROS2_HEADERS}
  ${CUR_DDS_ROS2_LIBS}
  control_msgs::control_msgs__rosidl_typesupport_cpp
  control_msgs::control_msgs__rosidl_typesupport_introspection_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_introspection_cpp)

if(TARGET ibmw::extensions::dds_types AND
   TARGET action_msgs::action_msgs__rosidl_generator_cpp AND
    TARGET action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp AND
    TARGET builtin_interfaces::builtin_interfaces__rosidl_generator_cpp AND
    TARGET builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp AND
    TARGET control_msgs::control_msgs__rosidl_generator_cpp AND
    TARGET control_msgs::control_msgs__rosidl_typesupport_fastrtps_cpp AND
    TARGET example_interfaces::example_interfaces__rosidl_generator_cpp AND
    TARGET example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp AND
    TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp AND
    TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp AND
    EXISTS ${CUR_SERVER_SOURCE} AND
    EXISTS ${CUR_GENERIC_SERVER_SOURCE} AND
    EXISTS ${CUR_COUNTDOWN_SERVER_SOURCE} AND
    EXISTS ${CUR_GRIPPER_SERVER_SOURCE})
  add_executable(${CUR_SERVER_TARGET_NAME} ${CUR_SERVER_SOURCE})
  add_executable(${CUR_SERVER_ALIAS_NAME} ALIAS ${CUR_SERVER_TARGET_NAME})

  target_include_directories(
    ${CUR_SERVER_TARGET_NAME}
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

  target_link_libraries(
    ${CUR_SERVER_TARGET_NAME}
    PRIVATE gflags::gflags
            ibmw::api::cpp
            ibmw::core::engine
            ibmw::extensions::dds_types
            ibmw::internal::ros2_action_dds
            ibmw::samples::cpp::ros2_action_fibonacci_common
            ${CUR_DDS_ROS2_HEADERS}
            ${CUR_DDS_ROS2_LIBS})

  set_target_properties(${CUR_SERVER_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_SERVER_TARGET_NAME})

  add_executable(${CUR_GENERIC_SERVER_TARGET_NAME} ${CUR_GENERIC_SERVER_SOURCE})
  add_executable(${CUR_GENERIC_SERVER_ALIAS_NAME} ALIAS ${CUR_GENERIC_SERVER_TARGET_NAME})

  target_include_directories(
    ${CUR_GENERIC_SERVER_TARGET_NAME}
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

  target_link_libraries(
    ${CUR_GENERIC_SERVER_TARGET_NAME}
    PRIVATE gflags::gflags
            ibmw::api::cpp
            ibmw::core::engine
            ibmw::extensions::dds_types
            ibmw::internal::ros2_action_dds
            ibmw::samples::cpp::ros2_action_fibonacci_common
            ${CUR_DDS_ROS2_HEADERS}
            ${CUR_DDS_ROS2_LIBS})

  set_target_properties(${CUR_GENERIC_SERVER_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_GENERIC_SERVER_TARGET_NAME})

  add_executable(${CUR_COUNTDOWN_SERVER_TARGET_NAME} ${CUR_COUNTDOWN_SERVER_SOURCE})
  add_executable(${CUR_COUNTDOWN_SERVER_ALIAS_NAME} ALIAS ${CUR_COUNTDOWN_SERVER_TARGET_NAME})

  target_include_directories(
    ${CUR_COUNTDOWN_SERVER_TARGET_NAME}
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

  target_link_libraries(
    ${CUR_COUNTDOWN_SERVER_TARGET_NAME}
    PRIVATE gflags::gflags
            ibmw::api::cpp
            ibmw::core::engine
            ibmw::extensions::dds_types
            ibmw::internal::ros2_action_dds
            ibmw::samples::cpp::ros2_action_fibonacci_common
            ${CUR_DDS_ROS2_HEADERS}
            ${CUR_DDS_ROS2_LIBS})

  set_target_properties(${CUR_COUNTDOWN_SERVER_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_COUNTDOWN_SERVER_TARGET_NAME})

  add_executable(${CUR_GRIPPER_SERVER_TARGET_NAME} ${CUR_GRIPPER_SERVER_SOURCE})
  add_executable(${CUR_GRIPPER_SERVER_ALIAS_NAME} ALIAS ${CUR_GRIPPER_SERVER_TARGET_NAME})

  target_include_directories(
    ${CUR_GRIPPER_SERVER_TARGET_NAME}
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

  target_link_libraries(
    ${CUR_GRIPPER_SERVER_TARGET_NAME}
    PRIVATE gflags::gflags
            ibmw::api::cpp
            ibmw::core::engine
            ibmw::extensions::dds_types
            ibmw::internal::ros2_action_dds
            ibmw::samples::cpp::ros2_action_fibonacci_common
            ${CUR_DDS_ROS2_HEADERS}
            ${CUR_DDS_ROS2_LIBS})

  set_target_properties(${CUR_GRIPPER_SERVER_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_GRIPPER_SERVER_TARGET_NAME})
else()
  message(STATUS
    "Skipping ${CUR_SERVER_TARGET_NAME}: requires ibmw::extensions::dds_types, ROS2 action generated type targets, and ${CUR_SERVER_SOURCE}")
endif()

if(IBMW_BUILD_WITH_ROS2)
  if(TARGET rclcpp::rclcpp AND TARGET rclcpp_action::rclcpp_action AND
     TARGET action_msgs::action_msgs__rosidl_generator_cpp AND
     TARGET action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp AND
     TARGET builtin_interfaces::builtin_interfaces__rosidl_generator_cpp AND
     TARGET builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp AND
     TARGET control_msgs::control_msgs__rosidl_generator_cpp AND
     TARGET control_msgs::control_msgs__rosidl_typesupport_cpp AND
     TARGET control_msgs::control_msgs__rosidl_typesupport_fastrtps_cpp AND
     TARGET control_msgs::control_msgs__rosidl_typesupport_introspection_cpp AND
     TARGET example_interfaces::example_interfaces__rosidl_generator_cpp AND
     TARGET example_interfaces::example_interfaces__rosidl_typesupport_cpp AND
     TARGET example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp AND
     TARGET example_interfaces::example_interfaces__rosidl_typesupport_introspection_cpp AND
     TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp AND
     TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp)
    add_executable(${CUR_CLIENT_TARGET_NAME} ${CUR_CLIENT_SOURCE})
    add_executable(${CUR_CLIENT_ALIAS_NAME} ALIAS ${CUR_CLIENT_TARGET_NAME})

    target_include_directories(
      ${CUR_CLIENT_TARGET_NAME}
      PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
              $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

    target_link_libraries(
      ${CUR_CLIENT_TARGET_NAME}
      PRIVATE gflags::gflags
              ibmw::core::engine
              rclcpp::rclcpp
              rclcpp_action::rclcpp_action
              ${CUR_NATIVE_ROS2_TYPES})

    set_target_properties(${CUR_CLIENT_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_CLIENT_TARGET_NAME})

    add_executable(${CUR_GRIPPER_CLIENT_TARGET_NAME} ${CUR_GRIPPER_CLIENT_SOURCE})
    add_executable(${CUR_GRIPPER_CLIENT_ALIAS_NAME} ALIAS ${CUR_GRIPPER_CLIENT_TARGET_NAME})

    target_include_directories(
      ${CUR_GRIPPER_CLIENT_TARGET_NAME}
      PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
              $<$<BOOL:${CUR_ROS2_RMW_INCLUDE_DIR}>:${CUR_ROS2_RMW_INCLUDE_DIR}>)

    target_link_libraries(
      ${CUR_GRIPPER_CLIENT_TARGET_NAME}
      PRIVATE gflags::gflags
              ibmw::core::engine
              rclcpp::rclcpp
              rclcpp_action::rclcpp_action
              ${CUR_NATIVE_ROS2_TYPES})

    set_target_properties(${CUR_GRIPPER_CLIENT_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_GRIPPER_CLIENT_TARGET_NAME})
  else()
    message(STATUS
      "Skipping ${CUR_CLIENT_TARGET_NAME}: requires rclcpp::rclcpp, rclcpp_action::rclcpp_action, and example_interfaces ROS2 action targets")
  endif()
else()
  message(STATUS
    "Skipping ${CUR_CLIENT_TARGET_NAME}: native ROS2 validation peer requires IBMW_BUILD_WITH_ROS2=ON")
endif()
