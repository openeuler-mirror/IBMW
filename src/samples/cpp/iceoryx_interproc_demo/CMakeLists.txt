# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iceoryx_interproc_demo: two-process publisher + subscriber over the
# Iceoryx shared-memory transport. Reuses sensor::CompressedImage IDL
# with crafted large payloads to exercise SHM auto-grow across processes
# connected via a shared iox-roudi daemon.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iceoryx_interproc_demo because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# NOTE: The legacy hand-written `compressed_image_cdr_impl.cc` (and its
# STATIC wrapper `iceoryx_interproc_demo_cdr_impl`) has been removed.
# CDR (de)serialization for sensor::CompressedImage is now provided by
# `ibmw::schema::sensor_idl_gencode` whose CMakeLists.txt declares
# `WRAP_IDLS CompressedImage`, so the *CdrAux.ipp inline definitions are
# already instantiated through the generated dds_types header that every
# module already includes. See docs/contributing/architecture-rules.md.

# --- Module static libraries ---

add_library(iceoryx_interproc_demo_publisher_module STATIC
  managed/publisher_module/publisher_module.cc)

target_include_directories(iceoryx_interproc_demo_publisher_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_interproc_demo_publisher_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

add_library(iceoryx_interproc_demo_subscriber_module STATIC
  managed/subscriber_module/subscriber_module.cc)

target_include_directories(iceoryx_interproc_demo_subscriber_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_interproc_demo_subscriber_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

# --- Apps (separate processes) ---

add_executable(iceoryx_interproc_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(iceoryx_interproc_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_interproc_publisher_app PRIVATE
  ibmw::core::engine
  iceoryx_interproc_demo_publisher_module)

add_executable(iceoryx_interproc_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(iceoryx_interproc_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iceoryx_interproc_subscriber_app PRIVATE
  ibmw::core::engine
  iceoryx_interproc_demo_subscriber_module)

# --- Install resources ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iceoryx_interproc_demo only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iceoryx_interproc_publisher_app
          iceoryx_interproc_subscriber_app
          ibmw_extensions_iceoryx_ext)
