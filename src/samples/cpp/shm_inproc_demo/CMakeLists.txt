# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm_inproc_demo: iceoryx_inproc_demo on the self-developed shm transport
# (shm_ext). Same publisher/subscriber modules and app entry (built from the
# iceoryx_inproc_demo sources), different extension in the yaml:
# libibmw_shm_transport.so with the default driver name "shm". No RouDi
# daemon and no iceoryx library are needed.

if(NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip shm_inproc_demo because IBMW_BUILD_SHM_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

set(IOX_INPROC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iceoryx_inproc_demo)

# --- Module static libraries ---

add_library(shm_inproc_demo_publisher_module STATIC
  ${IOX_INPROC_DIR}/managed/publisher_module/publisher_module.cc)

target_include_directories(shm_inproc_demo_publisher_module
  PUBLIC ${IOX_INPROC_DIR}/managed)

target_link_libraries(shm_inproc_demo_publisher_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

add_library(shm_inproc_demo_subscriber_module STATIC
  ${IOX_INPROC_DIR}/managed/subscriber_module/subscriber_module.cc)

target_include_directories(shm_inproc_demo_subscriber_module
  PUBLIC ${IOX_INPROC_DIR}/managed)

target_link_libraries(shm_inproc_demo_subscriber_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::sensor_idl_gencode)

# --- App ---

add_executable(shm_inproc_app
  ${IOX_INPROC_DIR}/standalone/pubsub_app/main.cc)

target_compile_definitions(shm_inproc_app PRIVATE
  "IBMW_DEMO_TITLE=\"IBMW Shm Inproc Demo\""
  "IBMW_DEMO_TRANSPORT_NOTE=\"NOTE: self-developed shm transport, no RouDi daemon needed.\"")

target_include_directories(shm_inproc_app PRIVATE
  ${IOX_INPROC_DIR}/managed)

target_link_libraries(shm_inproc_app PRIVATE
  ibmw::core::engine
  shm_inproc_demo_publisher_module
  shm_inproc_demo_subscriber_module)

# --- Install resources ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "shm_inproc_demo only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS shm_inproc_app
          ibmw_extensions_shm_ext)
