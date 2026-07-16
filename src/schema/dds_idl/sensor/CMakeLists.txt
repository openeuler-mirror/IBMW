# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# DDS IDL - Sensor message types (ImuState, CompressedImage, JointState, TouchSensorState)

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Resolve dependency target names for common and geometry
get_namespace(PROTOCOLS_NAMESPACE)
string(REPLACE "::" "_" PROTOCOLS_NAMESPACE_UNDERLINE ${PROTOCOLS_NAMESPACE})

# Generate C++ code from IDL files
add_dds_idl_gencode_target(
  TARGET_NAME ${CUR_TARGET_NAME}_idl_gencode
  IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/ImuState.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/CompressedImage.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/JointState.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/TouchSensorState.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
  INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../geometry
  # Cross-package CdrAux.ipp injection: sensor types embed common::Header and
  # geometry::Vector3/Quaternion, so each sensor wrapper TU must see those
  # upstream gencode dirs to resolve their inline FastCDR specializations.
  UPSTREAM_GENCODE_PATHS
    ${CMAKE_CURRENT_BINARY_DIR}/../common/gencode
    ${CMAKE_CURRENT_BINARY_DIR}/../geometry/gencode
  # Opt-in CDR impl wrappers. ImuState / JointState / TouchSensorState contain
  # nested-struct sequences (sequence<JointState>, sequence<TouchSensorState>)
  # that trip the FastCDR v1 fallback to T::serialize member; only wrap the
  # benchmark-required CompressedImage for now.
  WRAP_IDLS CompressedImage
  DEPENDENCIES
    ${CUR_SUPERIOR_NAMESPACE}::common_idl_gencode
    ${CUR_SUPERIOR_NAMESPACE}::geometry_idl_gencode
)
add_library(${CUR_TARGET_ALIAS_NAME}_idl_gencode ALIAS ${CUR_TARGET_NAME}_idl_gencode)

# Install
if(IBMW_INSTALL)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DESTINATION "share"
    FILES_MATCHING
    PATTERN "*.idl")

  set_property(TARGET ${CUR_TARGET_NAME}_idl_gencode PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME}_idl_gencode)
  install(
    TARGETS ${CUR_TARGET_NAME}_idl_gencode
    EXPORT ${INSTALL_CONFIG_NAME}
    ARCHIVE DESTINATION lib
            FILE_SET HEADERS
            DESTINATION include/${CUR_TARGET_NAME}_idl_gencode)
endif()
