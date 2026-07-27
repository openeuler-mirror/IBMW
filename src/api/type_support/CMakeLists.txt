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
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)

# Add target
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Framework-owned TypeSupport adapters.  Generated package DSOs depend on
# these targets, never on transport extension targets.  The adapter headers
# contain the selected provider-specific implementation, while the dependency
# name and installed include root remain framework-owned.
add_library(ibmw_api_dds_type_support INTERFACE)
add_library(ibmw::api::dds_type_support ALIAS ibmw_api_dds_type_support)
target_link_libraries(
  ibmw_api_dds_type_support
  INTERFACE ${CUR_TARGET_NAME} ibmw::api::cpp)
if(IBMW_FASTCDR_PROVIDER_TARGET)
  target_link_libraries(ibmw_api_dds_type_support INTERFACE
                        ${IBMW_FASTCDR_PROVIDER_TARGET})
endif()
if(IBMW_FASTDDS_PROVIDER_TARGET)
  target_link_libraries(ibmw_api_dds_type_support INTERFACE
                        ${IBMW_FASTDDS_PROVIDER_TARGET})
endif()

add_library(ibmw_api_ros2_type_support INTERFACE)
add_library(ibmw::api::ros2_type_support ALIAS ibmw_api_ros2_type_support)
target_link_libraries(
  ibmw_api_ros2_type_support
  INTERFACE ${CUR_TARGET_NAME} ibmw::api::cpp)
if(IBMW_FASTCDR_PROVIDER_TARGET)
  target_link_libraries(ibmw_api_ros2_type_support INTERFACE
                        ${IBMW_FASTCDR_PROVIDER_TARGET})
endif()
if(IBMW_FASTDDS_PROVIDER_TARGET)
  target_link_libraries(ibmw_api_ros2_type_support INTERFACE
                        ${IBMW_FASTDDS_PROVIDER_TARGET})
endif()

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)
target_include_directories(
  ibmw_api_dds_type_support
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)
target_include_directories(
  ibmw_api_ros2_type_support
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Set link libraries of target
target_link_libraries(
  ${CUR_TARGET_NAME}
  INTERFACE ibmw::api::c)

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  set_property(TARGET ibmw_api_dds_type_support
               PROPERTY EXPORT_NAME ibmw::api::dds_type_support)
  set_property(TARGET ibmw_api_ros2_type_support
               PROPERTY EXPORT_NAME ibmw::api::ros2_type_support)
  install(
    TARGETS ${CUR_TARGET_NAME} ibmw_api_dds_type_support
            ibmw_api_ros2_type_support
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  add_library(ibmw_schema_support_v1_c_header_test OBJECT
              schema_support_v1_c_compile_test.c)
  target_link_libraries(ibmw_schema_support_v1_c_header_test
                        PRIVATE ibmw::api::type_support)
endif()
