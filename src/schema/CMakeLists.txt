# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

# ----------------------------------------------------------------------------
# Legacy FastCDR v1 CDR helpers for fastddsgen-generated wrapper TUs (WRAP_IDLS).
#
# WRAP_IDLS compiles the auto-generated *CdrAuxImpl.cc wrapper TUs that pull in
# `dds_types/util/fastcdr_v1_compat.h` (CDR-only, no FastDDS / no ibmw API
# dependency). Previously this target linked `ibmw::extensions::dds_types`
# directly, which created a reverse dependency: the schema layer (always built
# when IBMW_BUILD_PROTOCOLS=ON) required the dds_extension layer (gated by
# IBMW_BUILD_DDS_EXTENSION). That broke any build that wanted protocols
# without the dds_extension.
#
# This INTERFACE target re-exports just the single legacy/v1 CDR-only header
# from its physical location under src/extensions/transport/dds_types/, with no
# FastDDS headers and no ibmw::api::cpp link. It is linked only by the FastCDR
# v1 compatibility lane in DdsIdlGenCode.cmake. It MUST stay outside any
# IBMW_BUILD_DDS_EXTENSION / IBMW_BUILD_RUNTIME gate so that legacy WRAP_IDLS
# consumers can link it whenever schema is being built.
# ----------------------------------------------------------------------------
add_library(ibmw_schema_dds_cdr_helpers INTERFACE)
add_library(ibmw::schema::dds_cdr_helpers ALIAS ibmw_schema_dds_cdr_helpers)
target_include_directories(ibmw_schema_dds_cdr_helpers
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/extensions/transport>
    $<INSTALL_INTERFACE:include/ibmw>)
target_link_libraries(ibmw_schema_dds_cdr_helpers INTERFACE fastcdr)
if(IBMW_INSTALL)
  set_property(TARGET ibmw_schema_dds_cdr_helpers
    PROPERTY EXPORT_NAME ibmw::schema::dds_cdr_helpers)
  install(
    TARGETS ibmw_schema_dds_cdr_helpers
    EXPORT ${INSTALL_CONFIG_NAME})
  install(
    FILES ${CMAKE_SOURCE_DIR}/src/extensions/transport/dds_types/util/fastcdr_v1_compat.h
    DESTINATION include/ibmw/dds_types/util)
endif()

# DDS IDL protocols (primary type system)
if(IBMW_BUILD_PROTOCOLS)
  add_subdirectory(dds_idl/common)
  add_subdirectory(dds_idl/geometry)
  add_subdirectory(dds_idl/sensor)
  add_subdirectory(dds_idl/actuator)
endif()

# The example IDL schema is needed only by DDS/protocol-aware examples.  Keep
# bare examples (for example quickstart / engine_smoke_test) buildable in
# minimal cross configurations where IBMW_BUILD_PROTOCOLS=OFF and no target
# FastCDR prefix is available.
if(IBMW_BUILD_EXAMPLES AND IBMW_BUILD_PROTOCOLS)
  add_subdirectory(dds_idl/example)
endif()

# Extension protocols: Management extensions use inline C++ structs with nlohmann::json
# (defined in *_types.h within each extension source directory). No separate protocol
# build targets are needed — the old protobuf gencode directories have been removed.

# ROS2 type support protocols (rosidl build-time only, no rcl runtime)
if(IBMW_BUILD_ROS2_TYPES)
  # The cross `types` tier consumes installed message types only. Generating
  # IBMW's own rosidl package requires the fuller target rosidl toolchain
  # (ament_cmake and generators), which is intentionally absent from a full
  # product message prefix.
  if(IBMW_BUILD_PROTOCOLS
     AND (NOT CMAKE_CROSSCOMPILING OR IBMW_TARGET_ROS2_TIER STREQUAL "rosidl"))
    add_subdirectory(ros2/ibmw_msgs)
  endif()
endif()

# ROS2 rcl-dependent protocols (need rclcpp for rpc gencode)
if(IBMW_BUILD_WITH_ROS2)
  if(IBMW_BUILD_EXAMPLES)
    add_subdirectory(ros2/example_ros2)
  endif()

  if(IBMW_BUILD_ROS2_EXTENSION)
    add_subdirectory(extensions/ros2_ext_proto)
  endif()
endif()

# DDS extension proto: GenericMsg IDL for DDS extension generic message transport.
# Gate it on protocols so protocol-free DDS extension builds do not require
# fastddsgen / IDL codegen.
if(IBMW_BUILD_DDS_EXTENSION AND IBMW_BUILD_PROTOCOLS)
  add_subdirectory(extensions/dds_ext_proto)
endif()
