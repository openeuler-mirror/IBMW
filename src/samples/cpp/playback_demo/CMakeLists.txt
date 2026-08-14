# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Pre-recorded MCAP/SQLite3 bag playback demonstration.

if(NOT IBMW_BUILD_DDS_EXTENSION OR
   NOT IBMW_BUILD_PROTOCOLS OR
   NOT IBMW_BUILD_RECORD_PLAYBACK_EXTENSION)
  message(STATUS "Skip playback_demo: DDS, protocols or record/playback is disabled")
  return()
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
  message(FATAL_ERROR "playback_demo currently supports Linux only")
endif()

get_filename_component(CUR_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set_namespace()

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Keep native DDS TypeSupport details out of editable managed modules.
# This adapter is the sample's generated/framework-owned transport boundary.
set(PLAYBACK_DEMO_SUBSCRIBER_TARGET
    ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_generated_subscriber)
add_library(
  ${PLAYBACK_DEMO_SUBSCRIBER_TARGET} STATIC
  generated/playback_demo_subscriber.cc)
add_library(
  ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::generated_subscriber ALIAS
  ${PLAYBACK_DEMO_SUBSCRIBER_TARGET})

target_include_directories(
  ${PLAYBACK_DEMO_SUBSCRIBER_TARGET}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

target_link_libraries(
  ${PLAYBACK_DEMO_SUBSCRIBER_TARGET}
  PUBLIC ibmw::api::cpp
  PRIVATE ibmw::extensions::dds_types
          ibmw::schema::common_idl_gencode)

add_subdirectory(managed/subscriber_module)
add_subdirectory(package/playback_demo_pkg)

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/bags
          ${CMAKE_BINARY_DIR}/playback_demo_bags
  DEPENDS ibmw::core::main
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::playback_demo_pkg
          ibmw_schema_common_ts
          ibmw_dds_extension
          ibmw_extensions_record_playback_ext)
