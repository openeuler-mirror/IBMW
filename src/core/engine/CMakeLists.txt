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
file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})
set(LEGACY_TRANSPORT_DRIVER_FIXTURE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/transport/test/abi/v1/driver_fixture.cc)
set(LEGACY_TRANSPORT_DRIVER_FIXTURE_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/transport/test/abi/v1/transport_driver.h)
set(EXTENSION_V2_FIXTURE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/extension/test/extension_v2_fixture.cc)
list(REMOVE_ITEM src ${LEGACY_TRANSPORT_DRIVER_FIXTURE_SRC}
                     ${EXTENSION_V2_FIXTURE_SRC})

list(REMOVE_ITEM head_files ${LEGACY_TRANSPORT_DRIVER_FIXTURE_HEADER})

# Add target
add_library(${CUR_TARGET_NAME} STATIC)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set source file of target
target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
         $<INSTALL_INTERFACE:include/ibmw>)

# Set link libraries of target
target_link_libraries(
  ${CUR_TARGET_NAME}
  PUBLIC ${CMAKE_DL_LIBS}
         asio::asio
         yaml-cpp::yaml-cpp
         TBB::tbb
         backward_iface
         ibmw::api::cpp
         ibmw::core::extension_api
         ibmw::base)

# Add -Werror option
include(AddWerror)
add_werror(${CUR_TARGET_NAME})

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
            FILE_SET HEADERS
            DESTINATION include/ibmw/${CUR_DIR})
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_library(legacy_transport_driver_v1 MODULE
              ${LEGACY_TRANSPORT_DRIVER_FIXTURE_SRC})
  target_include_directories(
    legacy_transport_driver_v1
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/transport/test/abi/v1)
  target_link_libraries(legacy_transport_driver_v1 PRIVATE yaml-cpp::yaml-cpp)
  if(UNIX AND NOT APPLE)
    set(LEGACY_TRANSPORT_DRIVER_FIXTURE_MAP
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/test/abi/v1/exports.map)
    target_link_options(
      legacy_transport_driver_v1
      PRIVATE "LINKER:--exclude-libs,ALL"
              "LINKER:--version-script=${LEGACY_TRANSPORT_DRIVER_FIXTURE_MAP}")
    set_target_properties(
      legacy_transport_driver_v1
      PROPERTIES LINK_DEPENDS ${LEGACY_TRANSPORT_DRIVER_FIXTURE_MAP})
  endif()
  set_target_properties(
    legacy_transport_driver_v1
    PROPERTIES CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON)

  foreach(fixture IN ITEMS extension_legacy_fixture
                           extension_v2_fixture
                           extension_v2_bad_version_fixture
                           extension_v2_bad_size_fixture
                           extension_v2_destroy_throws_fixture)
    add_library(${fixture} MODULE ${EXTENSION_V2_FIXTURE_SRC})
    target_link_libraries(${fixture} PRIVATE ibmw::core::extension_api)
    set_target_properties(${fixture} PROPERTIES CXX_VISIBILITY_PRESET hidden
                                                VISIBILITY_INLINES_HIDDEN ON)
  endforeach()
  target_compile_definitions(extension_legacy_fixture
                             PRIVATE IBMW_EXT_FIXTURE_LEGACY)
  target_compile_definitions(extension_v2_bad_version_fixture
                             PRIVATE IBMW_EXT_FIXTURE_BAD_VERSION)
  target_compile_definitions(extension_v2_bad_size_fixture
                             PRIVATE IBMW_EXT_FIXTURE_BAD_SIZE)
  target_compile_definitions(extension_v2_destroy_throws_fixture
                             PRIVATE IBMW_EXT_FIXTURE_DESTROY_THROWS)

  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  add_dependencies(${CUR_TARGET_NAME}_test
                   legacy_transport_driver_v1
                   extension_legacy_fixture
                   extension_v2_fixture
                   extension_v2_bad_version_fixture
                   extension_v2_bad_size_fixture
                   extension_v2_destroy_throws_fixture)
  target_compile_definitions(
    ${CUR_TARGET_NAME}_test
    PRIVATE
      "LEGACY_TRANSPORT_DRIVER_FIXTURE_PATH=\"$<TARGET_FILE:legacy_transport_driver_v1>\"")
  target_compile_definitions(
    ${CUR_TARGET_NAME}_test
    PRIVATE
      "EXTENSION_LEGACY_FIXTURE_PATH=\"$<TARGET_FILE:extension_legacy_fixture>\""
      "EXTENSION_V2_FIXTURE_PATH=\"$<TARGET_FILE:extension_v2_fixture>\""
      "EXTENSION_V2_BAD_VERSION_FIXTURE_PATH=\"$<TARGET_FILE:extension_v2_bad_version_fixture>\""
      "EXTENSION_V2_BAD_SIZE_FIXTURE_PATH=\"$<TARGET_FILE:extension_v2_bad_size_fixture>\""
      "EXTENSION_V2_DESTROY_THROWS_FIXTURE_PATH=\"$<TARGET_FILE:extension_v2_destroy_throws_fixture>\"")
endif()
