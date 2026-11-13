# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm_interproc_demo: iceoryx_interproc_demo on the self-developed shm
# transport (shm_ext). Same publisher/subscriber modules and process entries
# (built from the iceoryx_interproc_demo sources), different extension in the
# yaml: libibmw_shm_transport.so with the default driver name "shm". The two
# processes meet in the shm root directory; no RouDi daemon and no iceoryx
# library are needed.

if(NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip shm_interproc_demo because IBMW_BUILD_SHM_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

set(IOX_INTERPROC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iceoryx_interproc_demo)
set(SHM_INTERPROC_DEMO_DEFINITIONS
  "IBMW_DEMO_TITLE=\"IBMW Shm Interproc Demo\""
  "IBMW_DEMO_TRANSPORT_NOTE=\"NOTE: self-developed shm transport, no RouDi daemon needed.\"")

# --- Module static libraries ---

add_library(shm_interproc_demo_publisher_module STATIC
  ${IOX_INTERPROC_DIR}/managed/publisher_module/publisher_module.cc)

target_include_directories(shm_interproc_demo_publisher_module
  PUBLIC ${IOX_INTERPROC_DIR}/managed)

target_link_libraries(shm_interproc_demo_publisher_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

add_library(shm_interproc_demo_subscriber_module STATIC
  ${IOX_INTERPROC_DIR}/managed/subscriber_module/subscriber_module.cc)

target_include_directories(shm_interproc_demo_subscriber_module
  PUBLIC ${IOX_INTERPROC_DIR}/managed)

target_link_libraries(shm_interproc_demo_subscriber_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

# --- Apps (separate processes) ---

add_executable(shm_interproc_publisher_app
  ${IOX_INTERPROC_DIR}/standalone/publisher_app/main.cc)

target_compile_definitions(shm_interproc_publisher_app PRIVATE
  ${SHM_INTERPROC_DEMO_DEFINITIONS})

target_include_directories(shm_interproc_publisher_app PRIVATE
  ${IOX_INTERPROC_DIR}/managed)

target_link_libraries(shm_interproc_publisher_app PRIVATE
  ibmw::core::engine
  shm_interproc_demo_publisher_module)

add_executable(shm_interproc_subscriber_app
  ${IOX_INTERPROC_DIR}/standalone/subscriber_app/main.cc)

target_compile_definitions(shm_interproc_subscriber_app PRIVATE
  ${SHM_INTERPROC_DEMO_DEFINITIONS})

target_include_directories(shm_interproc_subscriber_app PRIVATE
  ${IOX_INTERPROC_DIR}/managed)

target_link_libraries(shm_interproc_subscriber_app PRIVATE
  ibmw::core::engine
  shm_interproc_demo_subscriber_module)

# --- Install resources ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "shm_interproc_demo only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS shm_interproc_publisher_app
          shm_interproc_subscriber_app
          ibmw_extensions_shm_ext)
