# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Check if DDS extension is enabled
if(NOT IBMW_BUILD_DDS_EXTENSION)
  message(STATUS "Skip dds_native_demo because IBMW_BUILD_DDS_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Set namespace
set_namespace()

# Need FastCDR for the hand-written DDS type serialization
find_package(fastcdr REQUIRED)

# Publisher module
add_library(publisher_module_native STATIC
  managed/publisher_module/publisher_module.cc
)

target_include_directories(publisher_module_native PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(publisher_module_native PUBLIC
  ibmw::api::cpp
  ibmw::extensions::dds_types
  fastcdr
)

# Subscriber module
add_library(subscriber_module_native STATIC
  managed/subscriber_module/subscriber_module.cc
)

target_include_directories(subscriber_module_native PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(subscriber_module_native PUBLIC
  ibmw::api::cpp
  ibmw::extensions::dds_types
  fastcdr
)

# Main application
add_executable(dds_native_app
  standalone/dds_native_app.cc
)

target_include_directories(dds_native_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dds_native_app PRIVATE
  ibmw::core::engine
  publisher_module_native
  subscriber_module_native
)

# Copy config and DDS extension to build directory for easier testing
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
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/cfg ${CMAKE_BINARY_DIR}
  DEPENDS dds_native_app
          ibmw_dds_extension)
