# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT IBMW_BUILD_DDS_EXTENSION OR
   NOT IBMW_BUILD_PROTOCOLS OR
   NOT IBMW_BUILD_ICEORYX_EXTENSION OR
   NOT IBMW_BUILD_MQTT_EXTENSION OR
   NOT IBMW_BUILD_ZENOH_EXTENSION OR
   NOT IBMW_BUILD_NET_EXTENSION OR
   NOT IBMW_BUILD_RECORD_PLAYBACK_EXTENSION OR
   NOT IBMW_BUILD_WITH_ROS2 OR
   NOT IBMW_BUILD_ROS2_EXTENSION OR
   NOT IBMW_BUILD_ROS2_TYPES)
  message(STATUS "Skip record_smoke_test: complete record transport matrix is disabled")
  return()
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(STATUS "Skip record_smoke_test: Linux is required")
  return()
endif()

find_package(rosgraph_msgs REQUIRED)

get_filename_component(CUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set_namespace()
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(MODULE_TARGET ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_module)
set(PKG_TARGET ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_pkg)
set(TS_TARGET ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_ts)

add_library(
  ${MODULE_TARGET} STATIC
  managed/record_smoke_module.cc
  generated/record_smoke_cdr_transport.cc
  generated/record_smoke_ros2_transport.cc
  generated/record_smoke_json_transport.cc)
set_target_properties(${MODULE_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(${MODULE_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  ${MODULE_TARGET}
  PUBLIC ibmw::api::cpp
  PRIVATE yaml-cpp::yaml-cpp
          nlohmann_json::nlohmann_json
          ibmw::extensions::dds_types
          ibmw::extensions::ros2_types
          ibmw::schema::common_idl_gencode
          example_ros2::example_ros2__rosidl_generator_cpp
          example_ros2::example_ros2__rosidl_typesupport_cpp
          example_ros2::example_ros2__rosidl_typesupport_fastrtps_cpp
          example_ros2::example_ros2__rosidl_typesupport_introspection_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_generator_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_fastrtps_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_introspection_cpp)

add_library(${PKG_TARGET} SHARED package/record_smoke_pkg_main.cc)
target_include_directories(${PKG_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${PKG_TARGET} PRIVATE ibmw::api::pkg ${MODULE_TARGET})
set_target_properties(${PKG_TARGET} PROPERTIES OUTPUT_NAME record_smoke_pkg)

add_library(${TS_TARGET} SHARED type_support_pkg_main.cc)
target_include_directories(${TS_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  ${TS_TARGET}
  PRIVATE ibmw::api::dds_type_support
          ibmw::schema::common_idl_gencode
          nlohmann_json::nlohmann_json
          ibmw::api::ros2_type_support
          fastcdr
          example_ros2::example_ros2__rosidl_generator_cpp
          example_ros2::example_ros2__rosidl_typesupport_cpp
          example_ros2::example_ros2__rosidl_typesupport_fastrtps_cpp
          example_ros2::example_ros2__rosidl_typesupport_introspection_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_generator_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_fastrtps_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_introspection_cpp)
set_target_properties(${TS_TARGET} PROPERTIES OUTPUT_NAME record_smoke_ts)

add_executable(
  ibmw_record_smoke_payload_validator
  validate_record_payload.cc)
target_include_directories(
  ibmw_record_smoke_payload_validator
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          ${PROJECT_SOURCE_DIR}/src/extensions/management)
target_link_libraries(
  ibmw_record_smoke_payload_validator
  PRIVATE ibmw::core::engine
          ibmw::extensions::record_playback_ext
          ibmw::api::type_support
          ibmw::api::cpp
          ibmw::extensions::dds_types
          ibmw::extensions::ros2_types
          ibmw::schema::common_idl_gencode
          nlohmann_json::nlohmann_json
          fastcdr
          example_ros2::example_ros2__rosidl_generator_cpp
          example_ros2::example_ros2__rosidl_typesupport_cpp
          example_ros2::example_ros2__rosidl_typesupport_fastrtps_cpp
          example_ros2::example_ros2__rosidl_typesupport_introspection_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_generator_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_fastrtps_cpp
          rosgraph_msgs::rosgraph_msgs__rosidl_typesupport_introspection_cpp)

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/share/ament_index/resource_index/packages
  COMMAND ${CMAKE_COMMAND} -E make_directory
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/lib
  COMMAND ${CMAKE_COMMAND} -E touch
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/share/ament_index/resource_index/packages/example_ros2
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:example_ros2__rosidl_typesupport_cpp>
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/lib/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:example_ros2__rosidl_typesupport_fastrtps_cpp>
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/lib/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:example_ros2__rosidl_typesupport_introspection_cpp>
          ${CMAKE_BINARY_DIR}/record_smoke_ros2_runtime/lib/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/validate_record_smoke.sh
          ${CMAKE_BINARY_DIR}/validate_record_smoke.sh
  COMMAND ${CMAKE_COMMAND} -E touch
          ${CMAKE_BINARY_DIR}/record_smoke_required
  COMMAND chmod +x ${CMAKE_BINARY_DIR}/validate_record_smoke.sh
  DEPENDS ibmw::core::main
          ${PKG_TARGET}
          ${TS_TARGET}
          ibmw_record_smoke_payload_validator
          example_ros2__rosidl_typesupport_cpp
          example_ros2__rosidl_typesupport_fastrtps_cpp
          example_ros2__rosidl_typesupport_introspection_cpp
          ibmw_schema_common_ts
          ibmw_dds_extension
          ibmw_extensions_iceoryx_ext
          iox-roudi
          ibmw_extensions_mqtt_ext
          ibmw_extensions_zenoh_ext
          ibmw_extensions_net_ext
          ibmw_extensions_ros2_ext
          ibmw_extensions_record_playback_ext)

if(IBMW_BUILD_TESTS)
  add_test(
    NAME ibmw_record_smoke_fixture
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all)
  set_tests_properties(
    ibmw_record_smoke_fixture
    PROPERTIES FIXTURES_SETUP ibmw_record_smoke_fixture
               TIMEOUT 900
               RESOURCE_LOCK ibmw_bag_smoke)

  add_test(
    NAME ibmw_record_smoke
    COMMAND ${CMAKE_COMMAND} -E env
            BUILD_DIR=${CMAKE_BINARY_DIR}
            CFG_DIR=${CMAKE_BINARY_DIR}/cfg
            TMP_DIR=${CMAKE_BINARY_DIR}/ctest-record-smoke
            ${CMAKE_BINARY_DIR}/validate_record_smoke.sh)
  set_tests_properties(
    ibmw_record_smoke
    PROPERTIES FIXTURES_REQUIRED ibmw_record_smoke_fixture
               TIMEOUT 300
               RESOURCE_LOCK ibmw_bag_smoke
               WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
