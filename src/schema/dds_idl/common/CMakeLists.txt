# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# DDS IDL - Common message types (Header, Error, ModuleStatus)

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Generate C++ code from IDL files
add_dds_idl_gencode_target(
  TARGET_NAME ${CUR_TARGET_NAME}_idl_gencode
  IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/Header.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/Error.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/ModuleStatus.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  # Opt-in CDR impl wrappers for IDLs whose generated code compiles cleanly
  # under our FastCDR v1 compatibility layer. ModuleStatus is excluded because
  # it contains a union + nested-struct vectors that the py transformer does
  # not yet handle. Add IDLs here once a real downstream consumer needs them.
  WRAP_IDLS Header Error
)
add_library(${CUR_TARGET_ALIAS_NAME}_idl_gencode ALIAS ${CUR_TARGET_NAME}_idl_gencode)

# A real loadable DDS TypeSupport Package used by Bag Schema discovery.
# dds_types is available when DDS or Iceoryx support is enabled.
if(TARGET ibmw::extensions::dds_types)
  add_library(
    ${CUR_TARGET_NAME}_ts SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/type_support_pkg_main.cc)
  add_library(${CUR_TARGET_ALIAS_NAME}_ts ALIAS ${CUR_TARGET_NAME}_ts)
  target_link_libraries(
    ${CUR_TARGET_NAME}_ts
    PRIVATE ibmw::api::dds_type_support
            ${CUR_TARGET_NAME}_idl_gencode)
  set_target_properties(
    ${CUR_TARGET_NAME}_ts PROPERTIES OUTPUT_NAME "${CUR_DIR}_dds_ts")
endif()

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

  if(TARGET ${CUR_TARGET_NAME}_ts)
    install(TARGETS ${CUR_TARGET_NAME}_ts LIBRARY DESTINATION bin)
  endif()
endif()
