# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Check if DDS extension is enabled
if(NOT IBMW_BUILD_DDS_EXTENSION)
  message(STATUS "Skip dds_image_demo because IBMW_BUILD_DDS_EXTENSION is OFF")
  return()
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Set namespace
set_namespace()

# Use the provider resolved by the main IBMW build.

# Publisher module
add_library(image_publisher_module STATIC
  managed/image_publisher/image_publisher.cc
)

target_include_directories(image_publisher_module PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(image_publisher_module PUBLIC
  ibmw::api::cpp
  ibmw::extensions::dds_types
  ${IBMW_FASTCDR_PROVIDER_TARGET}
)

# Subscriber module
add_library(image_subscriber_module STATIC
  managed/image_subscriber/image_subscriber.cc
)

target_include_directories(image_subscriber_module PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(image_subscriber_module PUBLIC
  ibmw::api::cpp
  ibmw::extensions::dds_types
  ${IBMW_FASTCDR_PROVIDER_TARGET}
)

# Main application
add_executable(dds_image_app
  standalone/dds_image_app.cc
)

target_include_directories(dds_image_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dds_image_app PRIVATE
  ibmw::core::engine
  image_publisher_module
  image_subscriber_module
)

# Copy config and DDS extension to build directory for easier testing
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "Unsupported OS")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/cfg ${CMAKE_BINARY_DIR}
  DEPENDS dds_image_app
          ibmw_dds_extension)
