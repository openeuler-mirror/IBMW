# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_library(ibmw_discovery_dds SHARED dds_discovery_provider.cc)
target_include_directories(ibmw_discovery_dds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(ibmw_discovery_dds PRIVATE ibmw::core::extension_api
  ${IBMW_FASTDDS_PROVIDER_TARGET} ${IBMW_FASTCDR_PROVIDER_TARGET})
set_target_properties(ibmw_discovery_dds PROPERTIES
  OUTPUT_NAME ibmw_discovery_dds LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
add_werror(ibmw_discovery_dds)
if(IBMW_INSTALL)
  install(TARGETS ibmw_discovery_dds LIBRARY DESTINATION bin COMPONENT discovery)
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_discovery_dds_test dds_discovery_provider_test.cc)
  target_include_directories(ibmw_discovery_dds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  target_link_libraries(ibmw_discovery_dds_test PRIVATE ibmw_discovery_dds
    ibmw::core::extension_api dds_ext_proto ibmw::extensions::dds_types GTest::gtest_main)
  add_werror(ibmw_discovery_dds_test)
  add_test(NAME ibmw_discovery_dds_test COMMAND ibmw_discovery_dds_test)
  set_tests_properties(ibmw_discovery_dds_test PROPERTIES TIMEOUT 30
    LABELS "integration;discovery;dds"
    ENVIRONMENT_MODIFICATION
      "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_topic_dds_subscription_test topic_subscription_test.cc)
  target_include_directories(ibmw_topic_dds_subscription_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  target_link_libraries(ibmw_topic_dds_subscription_test PRIVATE
    ibmw::core::engine dds_ext_proto ibmw::extensions::dds_types GTest::gtest_main)
  add_dependencies(ibmw_topic_dds_subscription_test ibmw_discovery_dds ibmw_dds_extension)
  add_werror(ibmw_topic_dds_subscription_test)
  add_test(NAME ibmw_topic_dds_subscription_test COMMAND ibmw_topic_dds_subscription_test)
  set_tests_properties(ibmw_topic_dds_subscription_test PROPERTIES TIMEOUT 60
    LABELS "integration;topic;dds"
    ENVIRONMENT "IBMW_DISCOVERY_PLUGIN_DIR=$<TARGET_FILE_DIR:ibmw_discovery_dds>"
    ENVIRONMENT_MODIFICATION
      "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
endif()
