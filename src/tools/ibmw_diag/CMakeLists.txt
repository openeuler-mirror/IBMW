# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(diag_cli_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../extensions/cli")

function(add_ibmw_diag_test component)
  set(target "ibmw_diag_${component}_test")
  add_executable(${target} "tests/${component}_test.cc" ${ARGN})
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../extensions")
  target_link_libraries(${target} PRIVATE
    ibmw::cli::command ibmw::core::extension_api ibmw::api::cpp
    nlohmann_json::nlohmann_json GTest::gtest_main)
  add_werror(${target})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES
    LABELS "ibmw_diag;unit" TIMEOUT 30)
  set_property(GLOBAL APPEND PROPERTY IBMW_DIAG_TEST_TARGETS ${target})
endfunction()

# Compile production units, never include/copy another suite's *_test.cc files.
add_ibmw_diag_test(topic
  "${diag_cli_dir}/topic/discovery_query.cc"
  "${diag_cli_dir}/topic/discovery_options.cc"
  "${diag_cli_dir}/topic/pub_options.cc"
  "${diag_cli_dir}/topic/echo_formatter.cc")
target_link_libraries(ibmw_diag_topic_test PRIVATE ibmw::core::engine)

add_ibmw_diag_test(service "${diag_cli_dir}/service/call_options.cc"
  "${diag_cli_dir}/service/discovery_options.cc"
  "${diag_cli_dir}/service/call_type_support.cc")
target_link_libraries(ibmw_diag_service_test PRIVATE ibmw::core::engine)

# Only the query boundary is replaced. No registry files or sockets are opened.
add_ibmw_diag_test(module "${diag_cli_dir}/module/module_command.cc"
  "${diag_cli_dir}/module/list_command.cc"
  "${diag_cli_dir}/module/info_command.cc")

# These executables deliberately do not link call_client.cc. The fake provides
# ExecuteCall, while real CLI parsing, request construction and validation run.
add_ibmw_diag_test(param support/rpc_fake.cc
  "${diag_cli_dir}/param/param_command.cc"
  "${diag_cli_dir}/service/call_options.cc")
target_include_directories(ibmw_diag_param_test PRIVATE "${diag_cli_dir}/service")
target_link_libraries(ibmw_diag_param_test PRIVATE
  ibmw::core::extension_support ibmw_parameter_types ${CMAKE_DL_LIBS})

add_ibmw_diag_test(log support/rpc_fake.cc
  "${diag_cli_dir}/log/log_client.cc"
  "${diag_cli_dir}/log/log_command.cc"
  "${diag_cli_dir}/service/call_options.cc")
target_include_directories(ibmw_diag_log_test PRIVATE "${diag_cli_dir}/service")
target_link_libraries(ibmw_diag_log_test PRIVATE
  ibmw::core::extension_support ibmw_log_control_types ${CMAKE_DL_LIBS})

add_ibmw_diag_test(doctor "${diag_cli_dir}/doctor/topic_check.cc"
  "${diag_cli_dir}/doctor/qos_check.cc" "${diag_cli_dir}/doctor/log_check.cc"
  "${diag_cli_dir}/doctor/module_check.cc")
target_include_directories(ibmw_diag_doctor_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../extensions/observability")

add_ibmw_diag_test(lifecycle)
target_link_libraries(ibmw_diag_lifecycle_test PRIVATE ibmw::core::engine)

get_property(diag_targets GLOBAL PROPERTY IBMW_DIAG_TEST_TARGETS)
add_custom_target(ibmw_diag DEPENDS ${diag_targets})
add_custom_target(check_ibmw_diag
  COMMAND ${CMAKE_CTEST_COMMAND} -C "$<CONFIG>" --output-on-failure --no-tests=error -L "^ibmw_diag$"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS ${diag_targets}
  USES_TERMINAL)

option(IBMW_DIAG_BUILD_FUNCTIONAL_TESTS "Build real-process IBMW functional regressions." OFF)
option(IBMW_DIAG_FULL_FUNCTIONAL_TESTS "Require and exercise every supported CLI backend and installation." OFF)
option(IBMW_DIAG_DDS_QOS_TESTS "Exercise native DDS QoS policies, including a standalone DDS 3 build." OFF)
if(IBMW_DIAG_FULL_FUNCTIONAL_TESTS AND NOT IBMW_DIAG_BUILD_FUNCTIONAL_TESTS)
  message(FATAL_ERROR "IBMW_DIAG_FULL_FUNCTIONAL_TESTS requires IBMW_DIAG_BUILD_FUNCTIONAL_TESTS=ON")
endif()
if(IBMW_DIAG_BUILD_FUNCTIONAL_TESTS)
  add_subdirectory(functional)
endif()
