# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# pubsub_demo: Demonstrates DDS IDL transport pub/sub with local and DDS drivers
# Replaces the original IBMW protobuf-based pubsub_demo with DDS IDL types.

if(NOT IBMW_BUILD_DDS_EXTENSION)
  message(STATUS "Skip pubsub_demo because IBMW_BUILD_DDS_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
#
# Historical note: a separate `pubsub_demo_cdr_impl` STATIC target used to
# compile a one-line example_event_cdr_impl.cc (just `#include
# "ExampleEventCdrAux.ipp"`). It was removed in 6.2.2.5 cleanup: the
# CdrAux.ipp wrapper TU is now emitted by `add_dds_idl_gencode_target(
# WRAP_IDLS ExampleEvent ...)` in src/schema/dds_idl/example/CMakeLists.txt,
# so every module that links `ibmw::schema::example_idl_gencode` already
# pulls the FastCDR symbols (or, more precisely, has the .ipp parsed at
# compile time -- the actual inline serialize/deserialize specialisations
# are emitted at the use sites in publisher_module.cc / subscriber_module.cc
# via include of `example_event_dds_types.h`).

# Publisher module
add_library(pubsub_demo_publisher_module STATIC
  managed/publisher_module/publisher_module.cc)

target_include_directories(pubsub_demo_publisher_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(pubsub_demo_publisher_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::example_idl_gencode)

# Subscriber module
add_library(pubsub_demo_subscriber_module STATIC
  managed/subscriber_module/subscriber_module.cc)

target_include_directories(pubsub_demo_subscriber_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(pubsub_demo_subscriber_module
  PUBLIC ibmw::api::cpp
         ibmw::extensions::dds_types
         ibmw::schema::example_idl_gencode)

# --- Pkg shared libraries (for ibmw pkg-mode loading) ---

# Publisher pkg
add_library(pubsub_demo_publisher_pkg SHARED
  package/publisher_pkg/pkg_main.cc)

target_link_libraries(pubsub_demo_publisher_pkg
  PRIVATE ibmw::api::pkg
          pubsub_demo_publisher_module)

set_target_properties(pubsub_demo_publisher_pkg PROPERTIES
  OUTPUT_NAME "pubsub_demo_publisher_pkg")

# Subscriber pkg
add_library(pubsub_demo_subscriber_pkg SHARED
  package/subscriber_pkg/pkg_main.cc)

target_link_libraries(pubsub_demo_subscriber_pkg
  PRIVATE ibmw::api::pkg
          pubsub_demo_subscriber_module)

set_target_properties(pubsub_demo_subscriber_pkg PROPERTIES
  OUTPUT_NAME "pubsub_demo_subscriber_pkg")

# --- App mode executables ---

# Combined pub/sub app (single process)
add_executable(pubsub_app
  standalone/pubsub_app/main.cc)

target_include_directories(pubsub_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(pubsub_app PRIVATE
  ibmw::core::engine
  pubsub_demo_publisher_module
  pubsub_demo_subscriber_module)

# Publisher-only app
add_executable(publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(publisher_app PRIVATE
  ibmw::core::engine
  pubsub_demo_publisher_module)

# Subscriber-only app
add_executable(subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(subscriber_app PRIVATE
  ibmw::core::engine
  pubsub_demo_subscriber_module)

# --- Install resources ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/win)
else()
  message(FATAL_ERROR "Unsupported OS")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS ibmw::core::main
          pubsub_demo_publisher_pkg
          pubsub_demo_subscriber_pkg
          pubsub_app
          publisher_app
          subscriber_app)
