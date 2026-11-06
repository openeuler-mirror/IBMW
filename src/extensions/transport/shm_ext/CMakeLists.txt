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
# Reporter V1 / core metrics bridge, as in iceoryx_ext: the real one only when
# the build switch is on, else a no-op without any host lookup.
list(REMOVE_ITEM src
  ${CMAKE_CURRENT_SOURCE_DIR}/reporter_bridge.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/reporter_bridge_stub.cc)
if(IBMW_ENABLE_REPORTER_V1)
  list(APPEND src ${CMAKE_CURRENT_SOURCE_DIR}/reporter_bridge.cc)
else()
  list(APPEND src ${CMAKE_CURRENT_SOURCE_DIR}/reporter_bridge_stub.cc)
endif()
# Needs a host executable exporting the reporter query: its own binary below.
set(REPORTER_HOST_BOUNDARY_TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/reporter_host_boundary_test.cc)
list(REMOVE_ITEM test_files ${REPORTER_HOST_BOUNDARY_TEST_SRC})

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
if(IBMW_ENABLE_REPORTER_V1)
  # dlopen(nullptr)/dlsym of the host reporter query.
  target_link_libraries(${CUR_TARGET_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()
# Installed, the selfdev headers sit in <prefix>/include (ibmw_selfdev_install);
# say so here rather than rely on another target adding that directory.
target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:$<TARGET_PROPERTY:ibmw::selfdev::shm,INTERFACE_INCLUDE_DIRECTORIES>>
         $<INSTALL_INTERFACE:include>)

include(AddWerror)
add_werror(${CUR_TARGET_NAME})
# dlclose must not unmap the plugin (ELF only). The selfdev runtime is linked
# into it: the process-wide runtime and its ibmw_shm_ctl thread live until
# process exit, ~ShmExtension deliberately leaks a bus that is still in use,
# and thread_locals with destructors (registered via __cxa_thread_atexit) run
# in threads outside the plugin. musl unmaps on dlclose (SIGSEGV in
# ibmw_shm_ctl on the OHOS device). The engine never unloads extensions anyway.
if(NOT APPLE AND NOT WIN32)
  target_link_options(${CUR_TARGET_NAME} PRIVATE "LINKER:-z,nodelete")
endif()

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
  # selfdev's operator new replacement (test::FailAllocations) makes the
  # library refuse a publish on demand (EXT-53/54).
  get_target_property(selfdev_shm_source_dir ibmw_selfdev_shm SOURCE_DIR)
  target_sources(${CUR_TARGET_NAME}_test PRIVATE ${selfdev_shm_source_dir}/test/fail_alloc.cc)
  target_include_directories(${CUR_TARGET_NAME}_test PRIVATE ${selfdev_shm_source_dir}/test)
  target_compile_definitions(${CUR_TARGET_NAME}_test
    PRIVATE IBMW_SHM_EXT_PEER_PATH="$<TARGET_FILE:ibmw_shm_ext_peer>")

  # Reporter V1 host boundary: the executable is the host (exports the reporter
  # query) and compiles the plugin sources in, so ShmTransportDriver reports
  # into the real core registry; it also dlopens the plugin itself.
  if(UNIX AND NOT APPLE)
    get_target_property(shm_reporter_test_sources ${CUR_TARGET_NAME} SOURCES)
    get_target_property(shm_reporter_test_includes ${CUR_TARGET_NAME} INCLUDE_DIRECTORIES)
    get_target_property(shm_reporter_test_links ${CUR_TARGET_NAME} LINK_LIBRARIES)
    add_executable(ibmw_shm_reporter_host_boundary_test
                   ${REPORTER_HOST_BOUNDARY_TEST_SRC} ${shm_reporter_test_sources}
                   ${selfdev_shm_source_dir}/test/fail_alloc.cc)
    target_include_directories(ibmw_shm_reporter_host_boundary_test
      PRIVATE ${shm_reporter_test_includes} ${selfdev_shm_source_dir}/test)
    target_link_libraries(ibmw_shm_reporter_host_boundary_test
      PRIVATE ibmw::core::reporter_host_abi ${shm_reporter_test_links}
              GTest::gtest GTest::gtest_main ${CMAKE_DL_LIBS})
    target_compile_definitions(ibmw_shm_reporter_host_boundary_test PRIVATE
      "SHM_EXTENSION_PATH=\"$<TARGET_FILE:${CUR_TARGET_NAME}>\"")
    set_target_properties(ibmw_shm_reporter_host_boundary_test PROPERTIES ENABLE_EXPORTS ON)
    add_dependencies(ibmw_shm_reporter_host_boundary_test ${CUR_TARGET_NAME})
    add_test(NAME ibmw_shm_reporter_host_boundary_test
             COMMAND $<TARGET_FILE:ibmw_shm_reporter_host_boundary_test>)
  endif()
endif()

set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "ibmw_shm_transport")

# BEGIN shm_ext bench block (run_shm_ext_bench.sh / run_shm_ohos.sh copy it into a baseline tree)
# Loan/publish microbenchmark, never installed. Off with IBMW_BUILD_TESTS because
# that switch adds --coverage to everything, whose counters would be measured.
# The plugin sources are compiled in, as add_gtest_target does, so the binary can
# be fully static on OHOS.
option(IBMW_BUILD_SHM_EXT_BENCH "Build the shm_ext loan/publish microbenchmark" OFF)
if(IBMW_BUILD_SHM_EXT_BENCH)
  add_executable(ibmw_shm_ext_bench test_peer/shm_ext_bench.cc ${src})
  target_include_directories(ibmw_shm_ext_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(ibmw_shm_ext_bench PRIVATE ibmw::core::extension_api ibmw::selfdev::shm ibmw::api::cpp
                                                   ibmw::core::engine)
  add_werror(ibmw_shm_ext_bench)
  # With Reporter V1 the bench is the Reporter V1 host (run_shm_ohos.sh
  # metrics-bench): it exports the query, so the compiled-in shm_ext sources
  # count into its core metrics under IBMW_CORE_METRICS=counters. A tree without
  # the switch, or a baseline tree this block is copied into (no
  # reporter_host_abi target), builds the bench as before.
  if(IBMW_ENABLE_REPORTER_V1 AND TARGET ibmw::core::reporter_host_abi)
    target_link_libraries(ibmw_shm_ext_bench PRIVATE ibmw::core::reporter_host_abi ${CMAKE_DL_LIBS})
    target_compile_definitions(ibmw_shm_ext_bench PRIVATE IBMW_SHM_EXT_BENCH_REPORTER_HOST)
    set_target_properties(ibmw_shm_ext_bench PROPERTIES ENABLE_EXPORTS ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
      # No OHOS platform module: ENABLE_EXPORTS does not add export-dynamic there.
      target_link_options(ibmw_shm_ext_bench PRIVATE "LINKER:--export-dynamic")
      set(shm_ext_bench_verify_export
          ${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/cpp/retained_loan_e2e/verify_reporter_export.cmake)
      if(EXISTS ${shm_ext_bench_verify_export})
        add_custom_command(TARGET ibmw_shm_ext_bench POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -DIBMW_NM=${CMAKE_NM} -DIBMW_EXECUTABLE=$<TARGET_FILE:ibmw_shm_ext_bench>
                  -P "${shm_ext_bench_verify_export}"
          VERBATIM)
      endif()
    endif()
  endif()
endif()
# END shm_ext bench block

# Shared-object checks for run_shm_ohos.sh `dynamic`, never installed:
# ibmw_shm_ext_dyn_runner loads the plugin through the core ExtensionLoader in a
# publisher and a subscriber process (it does not link the plugin), and
# ibmw_shm_ext_bench_so is the microbenchmark linked against the plugin instead
# of compiling its sources in, so its hot path runs in the shared object.
option(IBMW_BUILD_SHM_EXT_DYN_CHECK "Build the shm_ext plugin runner and the bench linked against the plugin" OFF)
if(IBMW_BUILD_SHM_EXT_DYN_CHECK)
  add_executable(ibmw_shm_ext_dyn_runner test_peer/shm_ext_dyn_runner.cc)
  target_link_libraries(ibmw_shm_ext_dyn_runner PRIVATE ibmw::core::engine ibmw::api::cpp ${CMAKE_DL_LIBS})
  add_dependencies(ibmw_shm_ext_dyn_runner ${CUR_TARGET_NAME})
  add_werror(ibmw_shm_ext_dyn_runner)
  add_executable(ibmw_shm_ext_bench_so test_peer/shm_ext_bench.cc)
  target_link_libraries(ibmw_shm_ext_bench_so PRIVATE ${CUR_TARGET_NAME} ibmw::core::extension_api)
  target_compile_definitions(ibmw_shm_ext_bench_so PRIVATE IBMW_SHM_EXT_BENCH_SO)
  add_werror(ibmw_shm_ext_bench_so)
endif()

# OHOS test HAP for run_shm_ohos.sh `hap`, never installed. HAP processes cannot
# exec files, so the test programs are shared objects started as native child
# processes by the NAPI module ibmw_shm_ext_hap (test_peer/shm_ext_hap.cc):
# the EXT tests (needs IBMW_BUILD_TESTS; the peer is not available, its three
# tests are filtered out there) and the runner. OHOS SDK only (ace_napi, child_process).
option(IBMW_BUILD_SHM_EXT_HAP_CHECK "Build the shm_ext test libraries of the OHOS test HAP" OFF)
if(IBMW_BUILD_SHM_EXT_HAP_CHECK)
  add_library(ibmw_shm_ext_hap SHARED test_peer/shm_ext_hap.cc)
  # child_process: dlopen'ed, see shm_ext_hap.cc.
  target_link_libraries(ibmw_shm_ext_hap PRIVATE ace_napi.z ${CMAKE_DL_LIBS})
  add_werror(ibmw_shm_ext_hap)
  add_library(ibmw_shm_ext_hap_runner SHARED test_peer/shm_ext_dyn_runner.cc)
  target_link_libraries(ibmw_shm_ext_hap_runner PRIVATE ibmw::core::engine ibmw::api::cpp ${CMAKE_DL_LIBS})
  target_compile_definitions(ibmw_shm_ext_hap_runner PRIVATE IBMW_SHM_EXT_DYN_RUNNER_LIBRARY)
  add_werror(ibmw_shm_ext_hap_runner)
  if(IBMW_BUILD_TESTS AND test_files)
    # As add_gtest_target for a shared plugin, without gtest_main.
    set(hap_test ${CUR_TARGET_NAME}_hap_test)
    get_target_property(hap_test_sources ${CUR_TARGET_NAME} SOURCES)
    get_target_property(hap_test_includes ${CUR_TARGET_NAME} INCLUDE_DIRECTORIES)
    get_target_property(hap_test_links ${CUR_TARGET_NAME} LINK_LIBRARIES)
    get_target_property(selfdev_shm_source_dir ibmw_selfdev_shm SOURCE_DIR)
    add_library(${hap_test} SHARED ${test_files} ${hap_test_sources} test_peer/shm_ext_hap_test_main.cc
                                   ${selfdev_shm_source_dir}/test/fail_alloc.cc)
    target_include_directories(${hap_test} PRIVATE ${hap_test_includes} ${selfdev_shm_source_dir}/test)
    target_link_libraries(${hap_test} PRIVATE ${hap_test_links} GTest::gtest GTest::gmock)
    target_compile_definitions(${hap_test} PRIVATE IBMW_SHM_EXT_PEER_PATH="/nonexistent/ibmw_shm_ext_peer")
    add_werror(${hap_test})
  endif()
endif()
