# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})

add_library(${CUR_TARGET_NAME} STATIC subscriber_module.cc)

target_include_directories(${CUR_TARGET_NAME}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
         ${CMAKE_CURRENT_SOURCE_DIR}/../..
         ${CMAKE_SOURCE_DIR}/src/extensions/transport)

if(IBMW_BUILD_TESTS)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME}
    TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../benchmark_run_state_test.cc)
endif()

target_link_libraries(${CUR_TARGET_NAME}
  PUBLIC ibmw::api::cpp
         ibmw::core::engine
         ibmw::samples::cpp::transport_benchmark::bench_payload
         ibmw::samples::cpp::transport_benchmark::bench_common
         ibmw::extensions::iceoryx_ext
         ibmw::extensions::dds_types
         iceoryx_posh::iceoryx_posh)
