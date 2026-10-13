# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(service_cli_sources
    service_command.cc service_cli_provider.cc
    discovery_options.cc discovery_query.cc
    list_command.cc type_command.cc find_command.cc)

add_library(ibmw_service_cli_provider SHARED ${service_cli_sources})
# Discovery observers do not initialize an Engine or load business transports.
target_link_libraries(ibmw_service_cli_provider
  PRIVATE ibmw::cli::command ibmw::core::extension_support)
set_target_properties(ibmw_service_cli_provider PROPERTIES OUTPUT_NAME "ibmw_service_cli")
add_werror(ibmw_service_cli_provider)

set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_service_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_service_cli_provider LIBRARY DESTINATION bin)
endif()

function(add_service_discovery backend)
  set(target ibmw_service_discovery_${backend})
  add_library(${target} SHARED providers/${backend}_provider.cc)
  target_link_libraries(${target} PRIVATE ibmw::core::extension_support ${ARGN})
  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
  add_werror(${target})
  add_dependencies(ibmw_service_cli_provider ${target})
  if(IBMW_INSTALL)
    install(TARGETS ${target} LIBRARY DESTINATION bin COMPONENT discovery)
  endif()
endfunction()

if(IBMW_BUILD_DDS_EXTENSION)
  add_service_discovery(dds)
  add_dependencies(ibmw_service_discovery_dds ibmw_discovery_dds)
endif()
if(IBMW_BUILD_ROS2_EXTENSION)
  add_service_discovery(ros2 rclcpp::rclcpp)
endif()
if(IBMW_BUILD_NET_EXTENSION)
  add_service_discovery(http ibmw::extensions::net_base)
  target_sources(ibmw_service_discovery_http PRIVATE providers/http_resolver.cc)
  target_link_libraries(ibmw_service_discovery_http PRIVATE ${CMAKE_DL_LIBS})
  add_executable(ibmw_service_dns_helper providers/http_dns_helper.cc)
  set_target_properties(ibmw_service_dns_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_werror(ibmw_service_dns_helper)
  add_dependencies(ibmw_service_discovery_http ibmw_service_dns_helper)
  if(IBMW_INSTALL)
    install(TARGETS ibmw_service_dns_helper RUNTIME DESTINATION bin COMPONENT discovery)
  endif()
endif()
if(IBMW_BUILD_GRPC_EXTENSION)
  add_service_discovery(grpc ibmw::extensions::net_base nghttp2::nghttp2)
  target_include_directories(ibmw_service_discovery_grpc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc)
  target_sources(ibmw_service_discovery_grpc PRIVATE
    ../../rpc/grpc_ext/global.cc
    ../../rpc/grpc_ext/http2/session.cc ../../rpc/grpc_ext/http2/client_session.cc)
endif()
if(IBMW_BUILD_MQTT_EXTENSION)
  add_service_discovery(mqtt paho_mqtt_c::paho-mqtt3as-static)
endif()
if(IBMW_BUILD_ZENOH_EXTENSION)
  add_service_discovery(zenoh zenohc::lib ibmw_zenoh_discovery_protocol)
  target_include_directories(ibmw_service_discovery_zenoh PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../transport)
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
