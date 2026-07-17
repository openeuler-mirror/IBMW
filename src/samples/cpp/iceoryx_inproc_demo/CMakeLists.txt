# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iceoryx_inproc_demo: single-process publisher + subscriber over the
# Iceoryx shared-memory transport extension. Reuses sensor::CompressedImage
# IDL with crafted large payloads to exercise SHM auto-grow.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iceoryx_inproc_demo because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# NOTE: The legacy hand-written `compressed_image_cdr_impl.cc` (and its
# STATIC wrapper `iceoryx_inproc_demo_cdr_impl`) has been removed. CDR
# (de)serialization for sensor::CompressedImage is now provided by
# `ibmw::schema::sensor_idl_gencode` whose CMakeLists.txt declares
# `WRAP_IDLS CompressedImage`. See docs/contributing/architecture-rules.md.

# --- Module static libraries ---

add_library(iceoryx_inproc_demo_publisher_module STATIC
  managed/publisher_module/publisher_module.cc)

target_include_directories(iceoryx_inproc_demo_publisher_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_inproc_demo_publisher_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

add_library(iceoryx_inproc_demo_subscriber_module STATIC
  managed/subscriber_module/subscriber_module.cc)

target_include_directories(iceoryx_inproc_demo_subscriber_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_inproc_demo_subscriber_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

# --- App ---

add_executable(iceoryx_inproc_app
  standalone/pubsub_app/main.cc)

target_include_directories(iceoryx_inproc_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_inproc_app PRIVATE
  ibmw::core::engine
  iceoryx_inproc_demo_publisher_module
  iceoryx_inproc_demo_subscriber_module)

# --- Install resources ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iceoryx_inproc_demo only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iceoryx_inproc_app
          ibmw_extensions_iceoryx_ext)
