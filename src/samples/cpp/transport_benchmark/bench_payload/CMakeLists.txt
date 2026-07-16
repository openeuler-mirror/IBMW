# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# bench_payload: header-only helpers for filling/parsing benchmark payloads
# in both the zero-copy POD path and the standard CompressedImage path.
# Pure INTERFACE library -- no source files.

set_namespace()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

target_include_directories(${CUR_TARGET_NAME}
  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${CUR_TARGET_NAME}
  INTERFACE
    ibmw::base
    # bench_plain_payloads.h needs:
    #   - dds_types/util/{fastcdr_v1_compat,dds_type_support}.h
    #   - dds_types/ibmw_dds_types.h (for IBMW_DDS_REGISTER_CONTEXT_TYPE)
    #   - cpp/context/details/type_support.h (for MessageTypeSupportTraits)
    # dds_types already INTERFACE-propagates fastcdr + fastrtps.
    ibmw::extensions::dds_types)

if(IBMW_BUILD_TESTS)
  file(GLOB test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
  if(test_files)
    add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  endif()
endif()
