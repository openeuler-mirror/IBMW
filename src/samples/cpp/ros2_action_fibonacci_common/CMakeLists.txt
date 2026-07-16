# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

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
  INTERFACE ibmw::base
            ibmw::internal::ros2_action_dds)

if(TARGET ibmw::extensions::dds_types)
  target_link_libraries(${CUR_TARGET_NAME}
    INTERFACE ibmw::extensions::dds_types)
endif()

if(IBMW_BUILD_TESTS)
  file(GLOB test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
  if(test_files)
    add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  endif()
endif()
