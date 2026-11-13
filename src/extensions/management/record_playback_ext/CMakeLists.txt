# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE cli_support_src CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM cli_support_src ${test_files}
                     ${CMAKE_CURRENT_SOURCE_DIR}/record_playback_extension.cc)

set(runtime_support_src
    ${CMAKE_CURRENT_SOURCE_DIR}/global.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/mcap_impl.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/playback_action.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/record_action.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/runtime_playback_stream.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/service.cc)
list(REMOVE_ITEM cli_support_src ${runtime_support_src})

add_library(${CUR_TARGET_NAME}_runtime_support STATIC ${runtime_support_src})
add_library(${CUR_TARGET_ALIAS_NAME}_runtime_support ALIAS ${CUR_TARGET_NAME}_runtime_support)
set_target_properties(${CUR_TARGET_NAME}_runtime_support PROPERTIES
                      POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  ${CUR_TARGET_NAME}_runtime_support
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
          $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)

add_library(${CUR_TARGET_NAME}_cli_support STATIC ${cli_support_src})
add_library(${CUR_TARGET_ALIAS_NAME}_cli_support ALIAS ${CUR_TARGET_NAME}_cli_support)
set_target_properties(${CUR_TARGET_NAME}_cli_support PROPERTIES
                      POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  ${CUR_TARGET_NAME}_cli_support
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
          $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)

# Add target
add_library(${CUR_TARGET_NAME} SHARED)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set source file of target
target_sources(${CUR_TARGET_NAME}
               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/record_playback_extension.cc)

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
          $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>)

# Set link libraries of target
find_package(SQLite3 REQUIRED)
# TBB / backward are not used by record_playback sources; they stay on the
# link line only when the selfdev replacements are switched off, to preserve
# the previous link line. (Plain if(), not generator expressions, so exported
# target names are mapped correctly.)
set(_ibmw_record_playback_legacy_libs)
if(NOT IBMW_SELFDEV_CONCURRENT)
  list(APPEND _ibmw_record_playback_legacy_libs TBB::tbb)
endif()
if(NOT IBMW_SELFDEV_CRASH_HANDLER)
  list(APPEND _ibmw_record_playback_legacy_libs backward_iface)
endif()

target_link_libraries(
  ${CUR_TARGET_NAME}_cli_support
  PUBLIC ibmw::core::extension_api
          ibmw::core::extension_support
          ibmw::api::cpp
          ibmw::base
          ${CMAKE_DL_LIBS}
          asio::asio
          yaml-cpp::yaml-cpp
          ${_ibmw_record_playback_legacy_libs}
          nlohmann_json::nlohmann_json
          mcap::mcap
          SQLite::SQLite3)
target_link_libraries(
  ${CUR_TARGET_NAME}_runtime_support
  PUBLIC ibmw::core::extension_api
          ibmw::core::extension_support
          ibmw::api::cpp
          ibmw::base
          ${CMAKE_DL_LIBS}
          asio::asio
          yaml-cpp::yaml-cpp
          ${_ibmw_record_playback_legacy_libs}
          nlohmann_json::nlohmann_json
          mcap::mcap)
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE ${CUR_TARGET_NAME}_runtime_support
          ibmw::core::extension_api
          ibmw::core::extension_support
          ibmw::api::cpp
          ibmw::base)
if(UNIX AND NOT APPLE)
  target_link_options(${CUR_TARGET_NAME} PRIVATE "LINKER:--no-undefined")
endif()

# rosbag2 conversion is available only in ROS2-enabled builds.
if(IBMW_BUILD_WITH_ROS2)
  find_package(ament_index_cpp REQUIRED)
  find_package(rosbag2_cpp REQUIRED)
  find_package(rosbag2_storage REQUIRED)
  find_package(rosbag2_transport REQUIRED)
  target_compile_definitions(${CUR_TARGET_NAME}_cli_support
                             PRIVATE IBMW_HAS_ROSBAG2=1)
  target_link_libraries(
    ${CUR_TARGET_NAME}_cli_support
    PRIVATE rclcpp::rclcpp
            ament_index_cpp::ament_index_cpp
            rosbag2_cpp::rosbag2_cpp
            rosbag2_storage::rosbag2_storage
            rosbag2_transport::rosbag2_transport)
endif()

# Add -Werror option
include(AddWerror)
add_werror(${CUR_TARGET_NAME})
add_werror(${CUR_TARGET_NAME}_runtime_support)
add_werror(${CUR_TARGET_NAME}_cli_support)

# Set installation of target
if(IBMW_INSTALL)
  install(TARGETS ${CUR_TARGET_NAME} LIBRARY DESTINATION bin)
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  target_link_libraries(${CUR_TARGET_NAME}_test PRIVATE
                        ${CUR_TARGET_NAME}_runtime_support
                        ${CUR_TARGET_NAME}_cli_support
                        ibmw::core::engine
                        SQLite::SQLite3)

  file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/runtime-load-test.yaml
    CONTENT "ibmw:\n  extensions:\n    items:\n      - name: record_playback_extension\n        path: $<TARGET_FILE:${CUR_TARGET_NAME}>\n        options:\n          enable_runtime_agent: false\n  logging:\n    core_level: INFO\n    backends:\n      - type: console\n  scheduling:\n    executors:\n      - name: work_thread_pool\n        type: thread_pool\n        options:\n          thread_num: 1\n")
  add_test(
    NAME ${CUR_TARGET_NAME}_runtime_load_test
    COMMAND $<TARGET_FILE:ibmw_core_main> run
            ${CMAKE_CURRENT_BINARY_DIR}/runtime-load-test.yaml --duration=1)
  set_tests_properties(${CUR_TARGET_NAME}_runtime_load_test
                       PROPERTIES TIMEOUT 5)
endif()

# Set misc of target
set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "ibmw_record_playback")
