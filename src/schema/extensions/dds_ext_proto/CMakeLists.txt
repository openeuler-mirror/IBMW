# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# DDS Extension Protocol - GenericMsg IDL for DDS extension generic message transport

if(NOT IBMW_BUILD_DDS_EXTENSION)
  return()
endif()

# Generate C++ code from DDS GenericMsg IDL
add_dds_idl_gencode_target(
  TARGET_NAME dds_ext_proto
  IDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/DdsGenericMsg.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
)

# Install
if(IBMW_INSTALL)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DESTINATION "share"
    FILES_MATCHING
    PATTERN "*.idl")

  install(
    TARGETS dds_ext_proto
    EXPORT ${INSTALL_CONFIG_NAME}
    ARCHIVE DESTINATION lib
            FILE_SET HEADERS
            DESTINATION include/dds_ext_proto)
endif()
