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
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)

# Add target
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

add_library(ibmw_extension_support STATIC type_support_package_loader.cc)
add_library(ibmw::core::extension_support ALIAS ibmw_extension_support)
set_target_properties(ibmw_extension_support PROPERTIES
                      POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  ibmw_extension_support
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../api>
         $<INSTALL_INTERFACE:include/ibmw>)
target_link_libraries(
  ibmw_extension_support
  PUBLIC ibmw::core::extension_api ibmw::api::cpp ibmw::api::type_support
  PRIVATE ${CMAKE_DL_LIBS})

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../api>
            $<INSTALL_INTERFACE:include/ibmw>)

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
  set_property(TARGET ibmw_extension_support
               PROPERTY EXPORT_NAME ibmw::core::extension_support)
  install(TARGETS ibmw_extension_support EXPORT ${INSTALL_CONFIG_NAME}
          ARCHIVE DESTINATION lib)
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  add_library(ibmw_reporter_v1_c_header_test OBJECT
              ibmw_reporter_v1_c_compile_test.c)
  target_include_directories(ibmw_reporter_v1_c_header_test
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
endif()
