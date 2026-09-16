# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(topic_cli_sources
    topic_command.cc
    topic_cli_provider.cc
    discovery_options.cc
    discovery_query.cc
    list_command.cc
    type_command.cc
    find_command.cc
    info_command.cc)
add_library(ibmw_topic_cli_provider SHARED ${topic_cli_sources})
target_link_libraries(ibmw_topic_cli_provider
  PRIVATE ibmw::cli::command ibmw::core::extension_support)
set_target_properties(ibmw_topic_cli_provider PROPERTIES OUTPUT_NAME "ibmw_topic_cli")
add_werror(ibmw_topic_cli_provider)

set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_topic_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_topic_cli_provider LIBRARY DESTINATION bin)
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_topic_cli_test topic_command_test.cc discovery_options_test.cc
                 ${topic_cli_sources})
  target_link_libraries(ibmw_topic_cli_test
    PRIVATE ibmw::cli::command ibmw::core::extension_support GTest::gtest_main)
  add_werror(ibmw_topic_cli_test)
  add_test(NAME ibmw_topic_cli_test COMMAND ibmw_topic_cli_test)
  set_tests_properties(ibmw_topic_cli_test PROPERTIES TIMEOUT 30)
endif()
