# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(service_cli_sources
    service_command.cc service_cli_provider.cc
    discovery_options.cc discovery_query.cc
    list_command.cc type_command.cc find_command.cc)

add_library(ibmw_service_cli_provider SHARED ${service_cli_sources})
# Read-only registry queries do not initialize an Engine or load transport plugins.
target_link_libraries(ibmw_service_cli_provider
  PRIVATE ibmw::cli::command ibmw::core::extension_support)
set_target_properties(ibmw_service_cli_provider PROPERTIES OUTPUT_NAME "ibmw_service_cli")
add_werror(ibmw_service_cli_provider)

set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_service_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_service_cli_provider LIBRARY DESTINATION bin)
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_service_cli_test service_command_test.cc ${service_cli_sources})
  target_link_libraries(ibmw_service_cli_test
    PRIVATE ibmw::cli::command ibmw::core::engine GTest::gtest_main)
  add_werror(ibmw_service_cli_test)
  add_dependencies(ibmw_service_cli_test ibmw_core_main)
  target_compile_definitions(ibmw_service_cli_test PRIVATE
    IBMW_SERVICE_TEST_CLI="$<TARGET_FILE:ibmw_core_main>")
  add_test(NAME ibmw_service_cli_test COMMAND ibmw_service_cli_test)
  set_tests_properties(ibmw_service_cli_test PROPERTIES TIMEOUT 30)
endif()
