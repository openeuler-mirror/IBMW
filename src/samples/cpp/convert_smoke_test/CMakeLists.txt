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

find_package(rosbag2_cpp REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(
  ibmw_convert_smoke_ibmw_payload_validator
  validate_convert_payload.cc)
target_include_directories(
  ibmw_convert_smoke_ibmw_payload_validator
  PRIVATE ${PROJECT_SOURCE_DIR}/src/extensions/management)
target_link_libraries(
  ibmw_convert_smoke_ibmw_payload_validator
  PRIVATE ibmw::extensions::record_playback_ext
          ibmw::core::engine
          rclcpp::rclcpp
          std_msgs::std_msgs__rosidl_generator_cpp
          std_msgs::std_msgs__rosidl_typesupport_cpp)

add_executable(
  ibmw_convert_smoke_rosbag2_payload_validator
  validate_rosbag2_payload.cc)
target_link_libraries(
  ibmw_convert_smoke_rosbag2_payload_validator
  PRIVATE rosbag2_cpp::rosbag2_cpp
          rclcpp::rclcpp
          std_msgs::std_msgs__rosidl_generator_cpp
          std_msgs::std_msgs__rosidl_typesupport_cpp)

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
          ibmw_extensions_record_playback_ext
          ibmw_convert_smoke_ibmw_payload_validator
          ibmw_convert_smoke_rosbag2_payload_validator)

if(IBMW_BUILD_TESTS)
  add_test(
    NAME ibmw_convert_smoke_fixture
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target ibmw_samples_cpp_convert_smoke_test_build_all)
  set_tests_properties(
    ibmw_convert_smoke_fixture
    PROPERTIES FIXTURES_SETUP ibmw_convert_smoke_fixture
               TIMEOUT 900
               RESOURCE_LOCK ibmw_bag_smoke)

  add_test(
    NAME ibmw_convert_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            BUILD_DIR=${CMAKE_BINARY_DIR}
            BAGS_DIR=${CMAKE_BINARY_DIR}/convert_smoke_bags
            CFG_DIR=${CMAKE_BINARY_DIR}/convert_smoke_cfg
            ${CMAKE_BINARY_DIR}/validate_convert_smoke.sh)
  set_tests_properties(
    ibmw_convert_smoke
    PROPERTIES FIXTURES_REQUIRED ibmw_convert_smoke_fixture
               TIMEOUT 180
               RESOURCE_LOCK ibmw_bag_smoke
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
