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
list(REMOVE_ITEM src ${CMAKE_CURRENT_SOURCE_DIR}/reporter_query.cc)
if(NOT IBMW_ENABLE_CORE_METRICS)
  list(REMOVE_ITEM test_files ${CMAKE_CURRENT_SOURCE_DIR}/core_metrics_test.cc)
endif()

list(REMOVE_ITEM head_files ${LEGACY_TRANSPORT_DRIVER_FIXTURE_HEADER})

# Add target
add_library(${CUR_TARGET_NAME} STATIC)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Stable Reporter ABI belongs to an extension-capable host, not the static
# engine archive (which plugins also link). Hosts explicitly consume this
# object, guaranteeing extraction without giving plugins a private copy.
add_library(ibmw_reporter_host_abi_object OBJECT reporter_query.cc)
target_link_libraries(ibmw_reporter_host_abi_object PRIVATE ibmw::core::extension_api)
add_library(ibmw_reporter_host_abi_archive STATIC
            $<TARGET_OBJECTS:ibmw_reporter_host_abi_object>)
add_library(ibmw_reporter_host_abi INTERFACE)
add_library(ibmw::core::reporter_host_abi ALIAS ibmw_reporter_host_abi)
# Consumers commonly discover the ABI with dlsym, so no ordinary undefined
# reference exists to extract reporter_query.cc from a static archive.
target_link_libraries(ibmw_reporter_host_abi INTERFACE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,ibmw_reporter_host_abi_archive>"
  ibmw::core::extension_api)
if(IBMW_ENABLE_REPORTER_V1)
  target_compile_definitions(ibmw_reporter_host_abi_object PRIVATE IBMW_ENABLE_REPORTER_V1=1)
  target_link_libraries(ibmw_reporter_host_abi INTERFACE ${CUR_TARGET_NAME})
endif()

# Set source file of target
target_sources(${CUR_TARGET_NAME} PRIVATE ${src})
if(IBMW_ENABLE_CORE_METRICS)
  target_sources(${CUR_TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../observability/core_metrics.cc)
  target_compile_definitions(${CUR_TARGET_NAME} PUBLIC IBMW_ENABLE_CORE_METRICS=1)
  if(IBMW_ENABLE_REPORTER_V1)
    target_compile_definitions(${CUR_TARGET_NAME} PUBLIC IBMW_ENABLE_REPORTER_V1=1)
  endif()
endif()

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})
if(IBMW_ENABLE_CORE_METRICS)
  target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET core_metrics_headers TYPE HEADERS
                 BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../
                 FILES ${CMAKE_CURRENT_SOURCE_DIR}/../observability/core_metrics.h)
endif()

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
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
  set_property(TARGET ibmw_reporter_host_abi PROPERTY EXPORT_NAME ibmw::core::reporter_host_abi)
  install(TARGETS ibmw_reporter_host_abi ibmw_reporter_host_abi_archive
          EXPORT ${INSTALL_CONFIG_NAME}
          ARCHIVE DESTINATION lib)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  if(IBMW_ENABLE_CORE_METRICS)
    install(TARGETS ${CUR_TARGET_NAME} EXPORT ${INSTALL_CONFIG_NAME}
            LIBRARY DESTINATION lib ARCHIVE DESTINATION lib
            FILE_SET HEADERS DESTINATION include/ibmw/${CUR_DIR}
            FILE_SET core_metrics_headers DESTINATION include/ibmw)
  else()
    install(TARGETS ${CUR_TARGET_NAME} EXPORT ${INSTALL_CONFIG_NAME}
            LIBRARY DESTINATION lib ARCHIVE DESTINATION lib
            FILE_SET HEADERS DESTINATION include/ibmw/${CUR_DIR})
  endif()
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
