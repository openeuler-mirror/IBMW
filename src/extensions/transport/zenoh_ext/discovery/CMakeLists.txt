# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_library(ibmw_zenoh_discovery_protocol STATIC zenoh_discovery_protocol.cc)
set_target_properties(ibmw_zenoh_discovery_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ibmw_zenoh_discovery_protocol PUBLIC ibmw::core::extension_api PRIVATE jsoncpp::jsoncpp zenohc::lib)
add_werror(ibmw_zenoh_discovery_protocol)

add_library(ibmw_discovery_zenoh SHARED zenoh_discovery_provider.cc)
target_link_libraries(ibmw_discovery_zenoh PRIVATE ibmw_zenoh_discovery_protocol zenohc::lib)
set_target_properties(ibmw_discovery_zenoh PROPERTIES
  OUTPUT_NAME ibmw_discovery_zenoh LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
add_werror(ibmw_discovery_zenoh)
if(IBMW_INSTALL)
  install(TARGETS ibmw_discovery_zenoh LIBRARY DESTINATION bin COMPONENT discovery)
endif()

if(IBMW_BUILD_TESTS)
  # The business extension intentionally hides its implementation symbols.
  # Compile its registration path into this regression test.
  add_executable(ibmw_discovery_zenoh_test zenoh_discovery_test.cc
    zenoh_discovery_responder.cc zenoh_service_discovery_responder.cc
    ../zenoh_bus.cc ../zenoh_transport_driver.cc ../global.cc)
  target_include_directories(ibmw_discovery_zenoh_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  target_link_libraries(ibmw_discovery_zenoh_test PRIVATE ibmw_discovery_zenoh
    ibmw_zenoh_discovery_protocol ibmw::core::engine
    zenohc::lib jsoncpp::jsoncpp GTest::gtest_main)
  add_werror(ibmw_discovery_zenoh_test)
  add_dependencies(ibmw_discovery_zenoh_test ibmw_extensions_zenoh_ext)
  add_test(NAME ibmw_discovery_zenoh_test COMMAND ibmw_discovery_zenoh_test)
  set_tests_properties(ibmw_discovery_zenoh_test PROPERTIES TIMEOUT 45 LABELS "integration;discovery;zenoh")
endif()
