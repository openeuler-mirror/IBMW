# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# The command implementation is a separate provider DSO. It is intentionally
# Core-dependent because it owns the standalone bag CLI's temporary Engine
# instances; the recording runtime extension remains a separate DSO.
add_library(ibmw_bag_cli_provider SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/bag_command.cc
            ${CMAKE_CURRENT_SOURCE_DIR}/bag_cli_provider.cc)
add_library(ibmw::extensions::bag_cli_provider ALIAS ibmw_bag_cli_provider)
target_include_directories(
  ibmw_bag_cli_provider
  PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/extensions/management>
          $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)
find_package(SQLite3 REQUIRED)
target_link_libraries(
  ibmw_bag_cli_provider
  PRIVATE ibmw::extensions::record_playback_ext_cli_support
          ibmw::extensions::record_playback_ext_runtime_support
          ibmw::core::extension_api
          ibmw::core::engine
          ibmw::api::cpp
          ibmw::base
          nlohmann_json::nlohmann_json
          mcap::mcap
          SQLite::SQLite3)
if(IBMW_BUILD_WITH_ROS2)
  find_package(ament_index_cpp REQUIRED)
  find_package(rosbag2_cpp REQUIRED)
  find_package(rosbag2_storage REQUIRED)
  find_package(rosbag2_transport REQUIRED)
  target_compile_definitions(ibmw_bag_cli_provider
                             PRIVATE IBMW_HAS_ROSBAG2=1)
  target_link_libraries(
    ibmw_bag_cli_provider
    PRIVATE rclcpp::rclcpp
            ament_index_cpp::ament_index_cpp
            rosbag2_cpp::rosbag2_cpp
            rosbag2_storage::rosbag2_storage
            rosbag2_transport::rosbag2_transport)
endif()
add_werror(ibmw_bag_cli_provider)
set_target_properties(ibmw_bag_cli_provider
                      PROPERTIES OUTPUT_NAME "ibmw_bag_cli")

set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_bag_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_bag_cli_provider LIBRARY DESTINATION bin)
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_bag_cli_test bag_command_test.cc)
  target_link_libraries(ibmw_bag_cli_test
    PRIVATE ibmw_bag_cli_provider ibmw::core::extension_api GTest::gtest_main)
  add_werror(ibmw_bag_cli_test)
  add_test(NAME ibmw_bag_cli_test COMMAND ibmw_bag_cli_test)
  set_tests_properties(ibmw_bag_cli_test PROPERTIES TIMEOUT 30)
endif()

if(IBMW_BUILD_TESTS AND IBMW_BUILD_ZENOH_EXTENSION)
  add_library(ibmw_auto_schema_package MODULE test/schema_package.cpp)
  target_link_libraries(ibmw_auto_schema_package PRIVATE ibmw::api::type_support)
  set(_zenoh_test_source ${PROJECT_SOURCE_DIR}/src/extensions/transport/zenoh_ext)
  add_executable(ibmw_bag_auto_schema_test auto_schema_record_test.cc
    ${_zenoh_test_source}/zenoh_bus.cc ${_zenoh_test_source}/global.cc
    ${_zenoh_test_source}/discovery/zenoh_discovery_responder.cc)
  target_include_directories(ibmw_bag_auto_schema_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/extensions/transport ${PROJECT_SOURCE_DIR}/src/extensions/management)
  target_link_libraries(ibmw_bag_auto_schema_test PRIVATE
    ibmw::core::engine ibmw::extensions::record_playback_ext_cli_support
    ibmw::extensions::record_playback_ext_runtime_support
    ibmw_zenoh_discovery_protocol zenohc::lib jsoncpp::jsoncpp GTest::gtest_main)
  target_compile_definitions(ibmw_bag_auto_schema_test PRIVATE
    IBMW_RECORD_TEST_EXECUTABLE="$<TARGET_FILE:ibmw_core_main>"
    ZENOH_RECORD_EXTENSION="$<TARGET_FILE:ibmw_extensions_zenoh_ext>"
    AUTO_SCHEMA_PACKAGE="$<TARGET_FILE:ibmw_auto_schema_package>")
  add_dependencies(ibmw_bag_auto_schema_test ibmw_core_main ibmw_auto_schema_package
    ibmw_bag_cli_provider ibmw_extensions_zenoh_ext)
  add_werror(ibmw_bag_auto_schema_test)
  add_test(NAME ibmw_bag_auto_schema_test COMMAND ibmw_bag_auto_schema_test)
  set_tests_properties(ibmw_bag_auto_schema_test PROPERTIES TIMEOUT 30 LABELS "integration;bag;zenoh")
endif()
