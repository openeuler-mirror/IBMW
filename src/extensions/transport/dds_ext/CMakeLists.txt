# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT IBMW_BUILD_DDS_EXTENSION)
  return()
endif()

if(NOT IBMW_FASTCDR_PROVIDER_TARGET OR NOT IBMW_FASTDDS_PROVIDER_TARGET)
  message(FATAL_ERROR "dds_ext requires the explicitly resolved IBMW DDS provider targets")
endif()

# Extension library
set(DDS_EXTENSION_SOURCES
  global.cc
  dds_extension.cc
  dds_bus.cc
  dds_transport_driver.cc
  dds_rpc_driver.cc
)

file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)

add_library(ibmw_dds_extension SHARED ${DDS_EXTENSION_SOURCES})

target_include_directories(ibmw_dds_extension
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
)

target_link_libraries(ibmw_dds_extension
  PRIVATE
    ibmw::core::extension_api
    ibmw::api::cpp
    ibmw::core::engine
    ibmw::base
    yaml-cpp
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET}
)

# Add ROS2 interface library support if available
if(TARGET ibmw::extensions::ros2_types)
  target_compile_definitions(ibmw_dds_extension PRIVATE
    IBMW_DDS_EXTENSION_HAS_ROS2_SUPPORT
  )
  if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
    target_link_libraries(ibmw_dds_extension
      PRIVATE
        ibmw::extensions::ros2_types_impl
    )
    message(STATUS "DDS Extension: ROS2 support enabled (cross-rosidl mode; public ros2_types interface kept off shared target)")
  else()
    target_link_libraries(ibmw_dds_extension
      PRIVATE
        ibmw::extensions::ros2_types
        ibmw::extensions::ros2_types_impl
    )
    message(STATUS "DDS Extension: ROS2 support enabled (decoupled mode)")
  endif()
endif()

set_target_properties(ibmw_dds_extension PROPERTIES
  OUTPUT_NAME ibmw_dds_transport
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  BUILD_RPATH ""
  INSTALL_RPATH ""
  INSTALL_RPATH_USE_LINK_PATH FALSE
  SKIP_BUILD_RPATH TRUE
  BUILD_WITH_INSTALL_RPATH FALSE
)

# Install
install(TARGETS ibmw_dds_extension
  EXPORT ${INSTALL_CONFIG_NAME}
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

# Public alias so downstream (incl. ibmw-gen generated `loaned_publisher`
# modules) can link the SHARED extension and pick up DdsTransportDriver
# typeinfo + member function symbols. Loading-as-plugin via `extensions:`
# config still works in parallel.
add_library(ibmw::extensions::dds_extension ALIAS ibmw_dds_extension)
set_property(TARGET ibmw_dds_extension
  PROPERTY EXPORT_NAME ibmw::extensions::dds_extension)

# ---------------------------------------------------------------------------
# Headers-only INTERFACE target — exposes dds_ext public headers
# (dds_transport_driver.h, ros2_style_api.h, loaned_message.h, etc.) so
# downstream out-of-tree projects (notably ibmw-gen generated `loaned_publisher`
# modules) can compile against them via:
#     find_package(ibmw)
#     target_link_libraries(my_module PRIVATE ibmw::extensions::dds_ext)
#
# Note: this target carries headers + fastdds/fastcdr transitive deps only;
# it does NOT link the SHARED ibmw_dds_extension library — that .so is loaded
# at runtime as a plugin via the engine's `extensions` config block.
# ---------------------------------------------------------------------------
file(GLOB dds_ext_public_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(ibmw_extensions_dds_ext INTERFACE)
add_library(ibmw::extensions::dds_ext ALIAS ibmw_extensions_dds_ext)

target_include_directories(ibmw_extensions_dds_ext
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include/ibmw>
)

target_sources(ibmw_extensions_dds_ext
  INTERFACE FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
  FILES ${dds_ext_public_headers}
)

target_link_libraries(ibmw_extensions_dds_ext
  INTERFACE
    ibmw::api::cpp
    ibmw::core::engine   # transitively exposes engine/transport/transport_driver.h
                         # which dds_transport_driver.h #includes
    ibmw::extensions::dds_extension   # SHARED lib carrying DdsTransportDriver
                                      # typeinfo & member-function symbols
                                      # required by downstream dynamic_cast +
                                      # template instantiations (e.g. ibmw-gen
                                      # `loaned_publisher` modules).
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET}
)

# ROS2 message support (optional, mirrors the SHARED ibmw_dds_extension above).
# `ros2_style_api.h` and the LoanedMessage Ros2Style wrappers `#include` ROS2
# message headers in template form, so downstream callers also need this.
if(TARGET ibmw::extensions::ros2_types)
  target_link_libraries(ibmw_extensions_dds_ext
    INTERFACE ibmw::extensions::ros2_types
  )
endif()

if(IBMW_INSTALL)
  set_property(TARGET ibmw_extensions_dds_ext
    PROPERTY EXPORT_NAME ibmw::extensions::dds_ext)
  install(
    TARGETS ibmw_extensions_dds_ext
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/dds_ext
  )
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ibmw_dds_extension TEST_SRC ${test_files})
endif()
