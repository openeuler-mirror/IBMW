include(CheckSymbolExists)
find_package(Threads REQUIRED)

check_symbol_exists(fexecve "unistd.h" IBMW_HAVE_FEXECVE)
if(NOT IBMW_HAVE_FEXECVE)
  message(FATAL_ERROR
    "mw_perf_ohos_capture_agent requires fexecve for validated-fd execution")
endif()

function(ibmw_add_capture_agent target)
  add_executable(${target} capture_agent.cc)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE
    _FILE_OFFSET_BITS=64 IBMW_HAVE_FEXECVE=1)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
      target_compile_options(${target} PRIVATE
        -Wno-error=unused-command-line-argument)
    endif()
  endif()
endfunction()

# The installed agent never has the fault injection test hooks.
ibmw_add_capture_agent(mw_perf_ohos_capture_agent)

if(IBMW_INSTALL)
  install(TARGETS mw_perf_ohos_capture_agent
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}"
    COMPONENT mw_perf_collect)
endif()

if(IBMW_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Python COMPONENTS Interpreter REQUIRED)
  add_executable(mw_perf_capture_fixture capture_fixture.cc)
  target_compile_features(mw_perf_capture_fixture PRIVATE cxx_std_20)
  target_compile_definitions(mw_perf_capture_fixture PRIVATE
    IBMW_VALID_TRACE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/valid_trace_fixture.py")
  # Test-only agent with the fault injection hooks; not installed.
  ibmw_add_capture_agent(mw_perf_ohos_capture_agent_testhooks)
  target_compile_definitions(mw_perf_ohos_capture_agent_testhooks PRIVATE
    IBMW_CAPTURE_TEST_HOOKS=1)
  target_link_libraries(mw_perf_capture_fixture PRIVATE Threads::Threads)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mw_perf_capture_fixture PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  set(_capture_acceptance_command
    ${Python_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/capture_acceptance_test.py
    $<TARGET_FILE:mw_perf_ohos_capture_agent_testhooks>
    $<TARGET_FILE:mw_perf_capture_fixture>)
  if(TARGET ibmw_dds_managed_exact_candidate)
    list(APPEND _capture_acceptance_command
      $<TARGET_FILE:ibmw_dds_managed_exact_candidate>
      $<TARGET_FILE:ibmw_dds_extension>)
  endif()
  add_test(NAME mw_perf_capture_acceptance COMMAND ${_capture_acceptance_command})
  set_tests_properties(mw_perf_capture_acceptance PROPERTIES TIMEOUT 30)
endif()
