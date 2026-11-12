# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

foreach(feature NET PARAMETER LOG_CONTROL)
  if(NOT IBMW_BUILD_${feature}_EXTENSION)
    message(FATAL_ERROR "IBMW functional tests require IBMW_BUILD_${feature}_EXTENSION=ON; they cannot run against mocks.")
  endif()
endforeach()
if(NOT UNIX)
  message(FATAL_ERROR "IBMW functional tests require POSIX processes and Unix-domain runtime sockets.")
endif()
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_library(ibmw_diag_functional_types SHARED ../fixtures/type_support.cc)
target_link_libraries(ibmw_diag_functional_types PRIVATE
  ibmw::api::cpp ibmw::api::type_support nlohmann_json::nlohmann_json)
add_werror(ibmw_diag_functional_types)

add_executable(ibmw_diag_runtime_fixture ../fixtures/runtime_server.cc)
target_link_libraries(ibmw_diag_runtime_fixture PRIVATE ibmw::core::engine)
add_werror(ibmw_diag_runtime_fixture)
add_executable(ibmw_diag_lifecycle_fixture ../fixtures/lifecycle_server.cc)
target_link_libraries(ibmw_diag_lifecycle_fixture PRIVATE ibmw::core::engine)
add_werror(ibmw_diag_lifecycle_fixture)

add_executable(ibmw_diag_typed_fixture ../fixtures/typed_runtime.cc)
target_link_libraries(ibmw_diag_typed_fixture PRIVATE ibmw::core::engine)
add_werror(ibmw_diag_typed_fixture)

set(functional_dependencies ibmw_core_main ibmw_diag_runtime_fixture
  ibmw_diag_lifecycle_fixture ibmw_diag_typed_fixture ibmw_diag_functional_types
  ibmw_extensions_net_ext ibmw_extensions_parameter_ext ibmw_extensions_log_control_ext)
add_custom_target(ibmw_diag_functional DEPENDS ${functional_dependencies})
foreach(component topic service param log module doctor lifecycle)
  add_test(NAME ibmw_diag_${component}_functional_test
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/scenarios.py"
      --case ${component}
      --cli "$<TARGET_FILE:ibmw_core_main>"
      --server "$<TARGET_FILE:ibmw_diag_runtime_fixture>"
      --lifecycle-server "$<TARGET_FILE:ibmw_diag_lifecycle_fixture>"
      --types "$<TARGET_FILE:ibmw_diag_functional_types>"
      --net "$<TARGET_FILE:ibmw_extensions_net_ext>"
      --parameter "$<TARGET_FILE:ibmw_extensions_parameter_ext>"
      --log "$<TARGET_FILE:ibmw_extensions_log_control_ext>"
      --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/${component}")
  set_tests_properties(ibmw_diag_${component}_functional_test PROPERTIES
    LABELS "ibmw_diag_functional;integration" TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()
if(IBMW_DIAG_FULL_FUNCTIONAL_TESTS)
  include(full.cmake)
endif()
if(IBMW_DIAG_FULL_FUNCTIONAL_TESTS OR IBMW_DIAG_DDS_QOS_TESTS)
  include(dds_qos.cmake)
endif()
add_custom_target(check_ibmw_diag_functional
  COMMAND ${CMAKE_CTEST_COMMAND} -C "$<CONFIG>" --output-on-failure --no-tests=error
    -L "^ibmw_diag_functional$"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  DEPENDS ${functional_dependencies}
  USES_TERMINAL)

# Faults at the actual DNS and HTTP wire boundaries, never an ExecuteCall mock.
add_library(ibmw_diag_slow_dns SHARED ../fixtures/slow_dns.cc)
target_link_libraries(ibmw_diag_slow_dns PRIVATE ${CMAKE_DL_LIBS})
add_werror(ibmw_diag_slow_dns)
add_executable(ibmw_diag_http_discovery_probe ../fixtures/http_discovery_probe.cc)
target_link_libraries(ibmw_diag_http_discovery_probe PRIVATE ibmw::core::engine)
add_werror(ibmw_diag_http_discovery_probe)
add_dependencies(ibmw_diag_functional ibmw_diag_slow_dns ibmw_diag_http_discovery_probe)
add_dependencies(check_ibmw_diag_functional ibmw_diag_slow_dns ibmw_diag_http_discovery_probe)
add_test(NAME ibmw_diag_http_discovery_deadline_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/http_discovery_deadline.py"
    --cli "$<TARGET_FILE:ibmw_core_main>" --probe "$<TARGET_FILE:ibmw_diag_http_discovery_probe>"
    --fault "$<TARGET_FILE:ibmw_diag_slow_dns>")
add_test(NAME ibmw_diag_http_response_validation_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/http_response_validation.py"
    --cli "$<TARGET_FILE:ibmw_core_main>")
set_tests_properties(ibmw_diag_http_discovery_deadline_test ibmw_diag_http_response_validation_test
  PROPERTIES LABELS "ibmw_diag_functional;integration" TIMEOUT 180 RUN_SERIAL TRUE)
