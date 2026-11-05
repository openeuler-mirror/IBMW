# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

add_library(ibmw_parameter_types SHARED)
target_include_directories(ibmw_parameter_types PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ibmw_parameter_types PUBLIC ibmw::api::cpp nlohmann_json::nlohmann_json)
if(IBMW_BUILD_DDS_EXTENSION)
  add_dds_idl_gencode_target(
    TARGET_NAME ibmw_parameter_dds_gencode
    IDL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/Parameter.idl
    GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    WRAP_IDLS Parameter)
  target_sources(ibmw_parameter_types PRIVATE parameter_dds_type_support.cc)
  target_link_libraries(ibmw_parameter_types PRIVATE ibmw_parameter_dds_gencode ibmw::api::dds_type_support)
else()
  target_sources(ibmw_parameter_types PRIVATE parameter_type_support.cc)
endif()
add_werror(ibmw_parameter_types)
set_target_properties(ibmw_parameter_types PROPERTIES INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
if(IBMW_INSTALL)
  install(TARGETS ibmw_parameter_types LIBRARY DESTINATION bin)
  install(FILES Parameter.idl DESTINATION share/ibmw/parameter)
endif()

if(IBMW_BUILD_PARAMETER_EXTENSION)
  add_library(ibmw_extensions_parameter_ext SHARED global.cc parameter_extension.cc service.cc)
  add_library(ibmw::extensions::parameter_ext ALIAS ibmw_extensions_parameter_ext)
  target_include_directories(ibmw_extensions_parameter_ext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(ibmw_extensions_parameter_ext PRIVATE
    ibmw::core::extension_api ibmw::core::engine ibmw_parameter_types)
  set_target_properties(ibmw_extensions_parameter_ext PROPERTIES OUTPUT_NAME "ibmw_parameter_service"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
  add_werror(ibmw_extensions_parameter_ext)
  if(IBMW_INSTALL)
    install(TARGETS ibmw_extensions_parameter_ext LIBRARY DESTINATION bin)
  endif()
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_parameter_service_test parameter_service_test.cc service.cc global.cc)
  target_link_libraries(ibmw_parameter_service_test PRIVATE ibmw::core::engine ibmw_parameter_types GTest::gtest_main)
  add_werror(ibmw_parameter_service_test)
  add_test(NAME ibmw_parameter_service_test COMMAND ibmw_parameter_service_test)
  set_tests_properties(ibmw_parameter_service_test PROPERTIES TIMEOUT 30)
endif()
