# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

add_library(ibmw_log_control_types SHARED)
target_include_directories(ibmw_log_control_types PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ibmw_log_control_types PUBLIC ibmw::api::cpp nlohmann_json::nlohmann_json)
if(IBMW_BUILD_DDS_EXTENSION)
  add_dds_idl_gencode_target(
    TARGET_NAME ibmw_log_control_dds_gencode
    IDL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/LogControl.idl
    GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    WRAP_IDLS LogControl)
  target_sources(ibmw_log_control_types PRIVATE log_control_dds_type_support.cc)
  target_link_libraries(ibmw_log_control_types PRIVATE ibmw_log_control_dds_gencode ibmw::api::dds_type_support)
else()
  target_sources(ibmw_log_control_types PRIVATE log_control_type_support.cc)
endif()
add_werror(ibmw_log_control_types)
set_target_properties(ibmw_log_control_types PROPERTIES INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
if(IBMW_INSTALL)
  install(TARGETS ibmw_log_control_types LIBRARY DESTINATION bin)
  install(FILES LogControl.idl DESTINATION share/ibmw/log_control)
endif()

if(IBMW_BUILD_LOG_CONTROL_EXTENSION)
  add_library(ibmw_extensions_log_control_ext SHARED global.cc log_control_extension.cc service.cc)
  add_library(ibmw::extensions::log_control_ext ALIAS ibmw_extensions_log_control_ext)
  target_include_directories(ibmw_extensions_log_control_ext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(ibmw_extensions_log_control_ext PRIVATE
    ibmw::core::extension_api ibmw::core::engine ibmw_log_control_types)
  set_target_properties(ibmw_extensions_log_control_ext PROPERTIES OUTPUT_NAME "ibmw_log_control"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
  add_werror(ibmw_extensions_log_control_ext)
  if(IBMW_INSTALL)
    install(TARGETS ibmw_extensions_log_control_ext LIBRARY DESTINATION bin)
  endif()
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_log_control_service_test log_control_service_test.cc service.cc global.cc)
  target_link_libraries(ibmw_log_control_service_test PRIVATE ibmw::core::engine ibmw_log_control_types GTest::gtest_main)
  add_werror(ibmw_log_control_service_test)
  add_test(NAME ibmw_log_control_service_test COMMAND ibmw_log_control_service_test)
  set_tests_properties(ibmw_log_control_service_test PROPERTIES TIMEOUT 30)
endif()
