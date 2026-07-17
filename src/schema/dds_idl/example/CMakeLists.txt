# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# DDS IDL - Example message types (ExampleCommon, ExampleEvent, ExampleRpc, ExampleBenchmark)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/ExampleCommon.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/ExampleEvent.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/ExampleRpc.idl
    ${CMAKE_CURRENT_SOURCE_DIR}/ExampleBenchmark.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  WRAP_IDLS ExampleCommon ExampleEvent ExampleRpc
)
add_library(${CUR_TARGET_ALIAS_NAME}_idl_gencode ALIAS ${CUR_TARGET_NAME}_idl_gencode)

# Generate RPC wrapper code from service definition YAML.
# The RPC gencode target links ibmw::extensions::dds_types (a real
# dds_extension API used by GetDdsMessageTypeSupport<>()), so the target
# is meaningful only when the DDS extension is being built. Skip
# materialization when the DDS extension is OFF; ALIAS users in samples
# already follow IBMW_HAS_DDS gating.
if(IBMW_BUILD_DDS_EXTENSION)
add_dds_idl_rpc_gencode_target(
  TARGET_NAME ${CUR_TARGET_NAME}_rpc_gencode
  SERVICE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/ExampleService.yaml
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/rpc_gencode
  IDL_FILE_STEM ExampleRpc
  DEP_IDL_TARGETS ${CUR_TARGET_NAME}_idl_gencode
)
add_library(${CUR_TARGET_ALIAS_NAME}_rpc_gencode ALIAS ${CUR_TARGET_NAME}_rpc_gencode)
endif()

# Install
if(IBMW_INSTALL)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DESTINATION "share"
    FILES_MATCHING
    PATTERN "*.idl"
    PATTERN "*.yaml")

  set_property(TARGET ${CUR_TARGET_NAME}_idl_gencode PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME}_idl_gencode)
  install(
    TARGETS ${CUR_TARGET_NAME}_idl_gencode
    EXPORT ${INSTALL_CONFIG_NAME}
    ARCHIVE DESTINATION lib
            FILE_SET HEADERS
            DESTINATION include/${CUR_TARGET_NAME}_idl_gencode)

  if(IBMW_BUILD_DDS_EXTENSION)
    set_property(TARGET ${CUR_TARGET_NAME}_rpc_gencode PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME}_rpc_gencode)
    install(
      TARGETS ${CUR_TARGET_NAME}_rpc_gencode
      EXPORT ${INSTALL_CONFIG_NAME}
      ARCHIVE DESTINATION lib
              FILE_SET HEADERS
              DESTINATION include/${CUR_TARGET_NAME}_rpc_gencode)
  endif()
endif()
