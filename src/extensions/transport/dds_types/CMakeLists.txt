# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# The DDS interface provides:
# - FastCDR-based serialization (dds_type_support.h)
# - GenericDdsPubSubType adapter (generic_dds_pubsub_type.h) that bridges
#   FastCDR serialize/deserialize to FastDDS TopicDataType for use by
#   the DDS transport driver.
# Both FastCDR and FastDDS (fastrtps) headers are needed by consumers.

if(NOT IBMW_FASTCDR_PROVIDER_TARGET OR NOT IBMW_FASTDDS_PROVIDER_TARGET)
  message(FATAL_ERROR "dds_types requires the explicitly resolved IBMW DDS provider targets")
endif()

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)

# Add target
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Set link libraries of target
# dds_type_support.h includes generic_dds_pubsub_type.h which needs FastDDS
# TopicDataType.hpp and TypeSupport.hpp headers, plus FastCDR for serialization.
target_link_libraries(
  ${CUR_TARGET_NAME}
  INTERFACE ${IBMW_FASTCDR_PROVIDER_TARGET}
            ibmw::api::cpp
            ibmw::internal)

# Add FastDDS dependency if available (needed for GenericDdsPubSubType)
target_link_libraries(${CUR_TARGET_NAME} INTERFACE ${IBMW_FASTDDS_PROVIDER_TARGET})

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
endif()
