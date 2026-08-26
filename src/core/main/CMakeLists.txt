# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
set(CLI_PROVIDER_FIXTURE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/test/cli_provider_fixture.cc)
list(REMOVE_ITEM src ${test_files} ${CLI_PROVIDER_FIXTURE_SRC})

# Add target
add_executable(${CUR_TARGET_NAME})
add_executable(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set source file of target
target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

# The runtime is the Reporter owner extensions resolve through dlopen(NULL).
# Export this opt-in host without globally polluting every executable.
set_target_properties(${CUR_TARGET_NAME} PROPERTIES ENABLE_EXPORTS ON)

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Set link libraries of target
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE ibmw::core::reporter_host_abi ibmw::core::engine)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(${CUR_TARGET_NAME} PROPERTIES LINK_FLAGS "-s")
  endif()
endif()

# Add -Werror option
include(AddWerror)
add_werror(${CUR_TARGET_NAME})

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    RUNTIME DESTINATION bin)
endif()

# Set misc of target
set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "ibmw")

if(IBMW_BUILD_TESTS AND test_files)
  add_library(ibmw_cli_provider_fixture MODULE ${CLI_PROVIDER_FIXTURE_SRC})
  target_link_libraries(ibmw_cli_provider_fixture PRIVATE ibmw::core::extension_api)
  add_executable(ibmw_cli_test ${test_files} ${src})
  target_compile_definitions(ibmw_cli_test PRIVATE IBMW_CLI_TEST
    IBMW_TEST_CLI_PROVIDER="$<TARGET_FILE:ibmw_cli_provider_fixture>")
  target_include_directories(ibmw_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ibmw_cli_test PRIVATE ibmw::core::engine GTest::gtest_main)
  add_dependencies(ibmw_cli_test ibmw_cli_provider_fixture)
  add_werror(ibmw_cli_test)
  add_test(NAME ibmw_cli_test COMMAND ibmw_cli_test)
  set_tests_properties(ibmw_cli_test PROPERTIES TIMEOUT 30)
endif()
