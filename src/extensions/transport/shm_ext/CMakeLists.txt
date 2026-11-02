# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Self-developed zero-copy shm transport (iceoryx replacement, no RouDi).
#
# CMake target:  ibmw_extensions_shm_ext   (SHARED)
# Alias        :  ibmw::extensions::shm_ext
#
# Same dual role as iceoryx_ext: plugin loaded by the engine, and direct-link
# target for samples that use ShmTransportDriver (EnsurePodTopic, loans).
# The selfdev library is linked statically and stays an implementation detail.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})
# test_peer/ is a standalone test helper, not part of the plugin.
list(FILTER src EXCLUDE REGEX "/test_peer/")

file(GLOB shm_ext_public_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(${CUR_TARGET_NAME} SHARED)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

target_sources(${CUR_TARGET_NAME} PRIVATE ${src})
target_sources(${CUR_TARGET_NAME}
  PUBLIC FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
  FILES ${shm_ext_public_headers}
)

target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include/ibmw>)

# The public headers include ibmw_selfdev/shm/shm.h, so its include path is
# propagated; the archive itself is PRIVATE.
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE ibmw::core::extension_api
          $<BUILD_INTERFACE:ibmw::selfdev::shm>
  PUBLIC  ibmw::api::cpp
          ibmw::core::engine)
# Installed, the selfdev headers sit in <prefix>/include (ibmw_selfdev_install);
# say so here rather than rely on another target adding that directory.
target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:$<TARGET_PROPERTY:ibmw::selfdev::shm,INTERFACE_INCLUDE_DIRECTORIES>>
         $<INSTALL_INTERFACE:include>)

include(AddWerror)
add_werror(${CUR_TARGET_NAME})

if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME}
    PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION bin
    FILE_SET HEADERS DESTINATION include/ibmw/shm_ext)
endif()

if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
  # Independent process speaking the IBMW frame format on raw selfdev shm,
  # used by the cross-process tests (EXT-22/23).
  add_executable(ibmw_shm_ext_peer test_peer/shm_ext_peer.cc)
  target_link_libraries(ibmw_shm_ext_peer PRIVATE ibmw::selfdev::shm)
  add_dependencies(${CUR_TARGET_NAME}_test ibmw_shm_ext_peer)
  target_compile_definitions(${CUR_TARGET_NAME}_test
    PRIVATE IBMW_SHM_EXT_PEER_PATH="$<TARGET_FILE:ibmw_shm_ext_peer>")
endif()

set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "ibmw_shm_transport")
