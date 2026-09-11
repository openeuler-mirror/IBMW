# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT IBMW_BUILD_DDS_EXTENSION)
  return()
endif()

if(NOT IBMW_FASTCDR_PROVIDER_TARGET OR NOT IBMW_FASTDDS_PROVIDER_TARGET)
  message(FATAL_ERROR "dds_ext requires the explicitly resolved IBMW DDS provider targets")
endif()

# Extension library
set(DDS_EXTENSION_SOURCES
  global.cc
  dds_extension.cc
  dds_bus.cc
  dds_transport_driver.cc
  dds_rpc_driver.cc
)

file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(FILTER test_files EXCLUDE REGEX "/dds_comm_trace_test\\.cc$")
list(FILTER test_files EXCLUDE REGEX "/dds_native_identity_roundtrip_test\\.cc$")
list(FILTER test_files EXCLUDE REGEX "/dds_managed_exact_candidate\\.cc$")
list(FILTER test_files EXCLUDE REGEX "/dds_extension_production_abi_test\\.cc$")

if(IBMW_ENABLE_COMM_TRACE)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()

function(ibmw_configure_dds_extension target output_name)
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api)
  target_link_libraries(${target} PRIVATE
    ibmw::core::extension_api
    ibmw::api::cpp
    ibmw::core::engine
    ibmw::base
    yaml-cpp
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET})
  if(IBMW_ENABLE_COMM_TRACE)
    target_sources(${target} PRIVATE dds_comm_trace.cc)
    target_compile_definitions(${target} PUBLIC IBMW_ENABLE_COMM_TRACE=1)
    target_link_libraries(${target} PRIVATE OpenSSL::Crypto)
  endif()
  if(TARGET ibmw::extensions::ros2_types)
    target_compile_definitions(${target} PRIVATE IBMW_DDS_EXTENSION_HAS_ROS2_SUPPORT)
    if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
      target_link_libraries(${target} PRIVATE ibmw::extensions::ros2_types_impl)
    else()
      target_link_libraries(${target} PRIVATE
        ibmw::extensions::ros2_types
        ibmw::extensions::ros2_types_impl)
    endif()
  endif()
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME ${output_name}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    BUILD_RPATH ""
    INSTALL_RPATH ""
    INSTALL_RPATH_USE_LINK_PATH FALSE
    SKIP_BUILD_RPATH TRUE
    BUILD_WITH_INSTALL_RPATH FALSE)
endfunction()

add_library(ibmw_dds_extension SHARED ${DDS_EXTENSION_SOURCES})
ibmw_configure_dds_extension(ibmw_dds_extension ibmw_dds_transport)

# Add ROS2 interface library support if available
if(TARGET ibmw::extensions::ros2_types)
  if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
    message(STATUS "DDS Extension: ROS2 support enabled (cross-rosidl mode; public ros2_types interface kept off shared target)")
  else()
    message(STATUS "DDS Extension: ROS2 support enabled (decoupled mode)")
  endif()
endif()

# Install
install(TARGETS ibmw_dds_extension
  EXPORT ${INSTALL_CONFIG_NAME}
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

# Public alias so downstream (incl. ibmw-gen generated `loaned_publisher`
# modules) can link the SHARED extension and pick up DdsTransportDriver
# typeinfo + member function symbols. Loading-as-plugin via `extensions:`
# config still works in parallel.
add_library(ibmw::extensions::dds_extension ALIAS ibmw_dds_extension)
set_property(TARGET ibmw_dds_extension
  PROPERTY EXPORT_NAME ibmw::extensions::dds_extension)

# ---------------------------------------------------------------------------
# Headers-only INTERFACE target — exposes dds_ext public headers
# (dds_transport_driver.h, ros2_style_api.h, loaned_message.h, etc.) so
# downstream out-of-tree projects (notably ibmw-gen generated `loaned_publisher`
# modules) can compile against them via:
#     find_package(ibmw)
#     target_link_libraries(my_module PRIVATE ibmw::extensions::dds_ext)
#
# Note: this target carries headers + fastdds/fastcdr transitive deps only;
# it does NOT link the SHARED ibmw_dds_extension library — that .so is loaded
# at runtime as a plugin via the engine's `extensions` config block.
# ---------------------------------------------------------------------------
file(GLOB dds_ext_public_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(ibmw_extensions_dds_ext INTERFACE)
add_library(ibmw::extensions::dds_ext ALIAS ibmw_extensions_dds_ext)

target_include_directories(ibmw_extensions_dds_ext
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include/ibmw>
)

target_sources(ibmw_extensions_dds_ext
  INTERFACE FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
  FILES ${dds_ext_public_headers}
)

target_link_libraries(ibmw_extensions_dds_ext
  INTERFACE
    ibmw::api::cpp
    ibmw::core::engine   # transitively exposes engine/transport/transport_driver.h
                         # which dds_transport_driver.h #includes
    ibmw::extensions::dds_extension   # SHARED lib carrying DdsTransportDriver
                                      # typeinfo & member-function symbols
                                      # required by downstream dynamic_cast +
                                      # template instantiations (e.g. ibmw-gen
                                      # `loaned_publisher` modules).
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET}
)

# ROS2 message support (optional, mirrors the SHARED ibmw_dds_extension above).
# `ros2_style_api.h` and the LoanedMessage Ros2Style wrappers `#include` ROS2
# message headers in template form, so downstream callers also need this.
if(TARGET ibmw::extensions::ros2_types)
  target_link_libraries(ibmw_extensions_dds_ext
    INTERFACE ibmw::extensions::ros2_types
  )
endif()

if(IBMW_INSTALL)
  set_property(TARGET ibmw_extensions_dds_ext
    PROPERTY EXPORT_NAME ibmw::extensions::dds_ext)
  install(
    TARGETS ibmw_extensions_dds_ext
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/dds_ext
  )
endif()

function(ibmw_configure_managed_exact_candidate target extension_target)
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src/core)
  target_link_libraries(${target} PRIVATE
    ${extension_target}
    dds_ext_proto
    ibmw::extensions::dds_types
    ibmw::core::engine
    ibmw::api::cpp
    ibmw::base
    yaml-cpp
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET})
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH
      "$ORIGIN;$<TARGET_FILE_DIR:${extension_target}>;$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>"
    INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
  if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_options(${target} PRIVATE
      "LINKER:-rpath,$ORIGIN,-rpath,$ORIGIN/../lib")
  endif()
endfunction()

# The production candidate is also used by the managed-exact integration test.
# Create it once when either production capture or that test lane requests it.
if(IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE OR
   IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE OR
   (IBMW_BUILD_TESTS AND IBMW_ENABLE_COMM_TRACE))
  add_executable(ibmw_dds_managed_exact_candidate
    dds_managed_exact_candidate.cc dds_bus.cc global.cc)
  ibmw_configure_managed_exact_candidate(
    ibmw_dds_managed_exact_candidate ibmw_dds_extension)

  if(IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE)
    if(NOT CMAKE_READELF OR NOT EXISTS "${CMAKE_READELF}")
      message(FATAL_ERROR
        "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE requires CMAKE_READELF to audit production RTTI dynamic symbols")
    endif()
    add_custom_command(TARGET ibmw_dds_managed_exact_candidate POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DREADELF=${CMAKE_READELF}
        "-DARTIFACTS=$<TARGET_FILE:ibmw_dds_managed_exact_candidate>;$<TARGET_FILE:ibmw_dds_extension>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_transport_rtti_symbols.cmake
      COMMENT "Auditing production DDS cross-DSO RTTI dynamic symbols"
      VERBATIM)
  endif()

  if(IBMW_INSTALL AND IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE)
    install(TARGETS
        ibmw_dds_managed_exact_candidate
        ibmw_dds_extension
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT dds_managed_exact_candidate
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT dds_managed_exact_candidate)
  endif()
endif()

# The fault candidate is a separate test-instrumented image.  It is enabled by
# an explicit production option so an OHOS image can ship the injection lane
# without enabling the full test suite.
if(IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE)
  add_library(ibmw_dds_extension_fault_candidate SHARED ${DDS_EXTENSION_SOURCES})
  ibmw_configure_dds_extension(
    ibmw_dds_extension_fault_candidate ibmw_dds_transport_fault_candidate)
  target_compile_definitions(ibmw_dds_extension_fault_candidate PRIVATE
    IBMW_COMM_TRACE_TEST_HOOKS=1)
  add_executable(ibmw_dds_managed_exact_fault_candidate
    dds_managed_exact_candidate.cc dds_bus.cc global.cc)
  ibmw_configure_managed_exact_candidate(
    ibmw_dds_managed_exact_fault_candidate ibmw_dds_extension_fault_candidate)
  target_compile_definitions(ibmw_dds_managed_exact_fault_candidate PRIVATE
    IBMW_MANAGED_EXACT_FAULT_CANDIDATE=1 IBMW_COMM_TRACE_TEST_HOOKS=1)
  if(IBMW_INSTALL)
    install(TARGETS ibmw_dds_managed_exact_fault_candidate
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT dds_managed_exact_fault_candidate)
    install(TARGETS ibmw_dds_extension_fault_candidate
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      COMPONENT dds_managed_exact_fault_candidate)
  endif()
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ibmw_dds_extension TEST_SRC ${test_files})
  set_tests_properties(ibmw_dds_extension_test PROPERTIES
    ENVIRONMENT_MODIFICATION
      "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_dds_native_identity_roundtrip_test
    dds_native_identity_roundtrip_test.cc)
  target_include_directories(ibmw_dds_native_identity_roundtrip_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/src/core)
  target_link_libraries(ibmw_dds_native_identity_roundtrip_test PRIVATE
    GTest::gtest_main
    dds_ext_proto
    ibmw::extensions::dds_types
    ${IBMW_FASTCDR_PROVIDER_TARGET}
    ${IBMW_FASTDDS_PROVIDER_TARGET}
    ${CMAKE_DL_LIBS})
  set_property(TARGET ibmw_dds_native_identity_roundtrip_test PROPERTY
    BUILD_RPATH
      "$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
  if(IBMW_DDS_PROVIDER MATCHES "^SOURCE_FASTDDS[23]$")
    file(REAL_PATH "${IBMW_THIRD_PARTY_SOURCE_PREFIX}" _ibmw_source_dds_prefix)
    target_compile_definitions(ibmw_dds_native_identity_roundtrip_test PRIVATE
      IBMW_EXPECTED_DDS_PREFIX="${_ibmw_source_dds_prefix}")
  endif()
  if(IBMW_ENABLE_COMM_TRACE)
    target_sources(ibmw_dds_native_identity_roundtrip_test PRIVATE
      global.cc
      dds_bus.cc
      dds_transport_driver.cc
      dds_comm_trace.cc)
    target_compile_definitions(ibmw_dds_native_identity_roundtrip_test PRIVATE
      IBMW_ENABLE_COMM_TRACE=1)
    target_link_libraries(ibmw_dds_native_identity_roundtrip_test PRIVATE
      OpenSSL::Crypto
      ibmw::core::engine
      ibmw::api::cpp
      ibmw::base
      yaml-cpp)
  endif()
  add_test(NAME ibmw_dds_native_identity_roundtrip_test
           COMMAND $<TARGET_FILE:ibmw_dds_native_identity_roundtrip_test>)
  set_tests_properties(ibmw_dds_native_identity_roundtrip_test PROPERTIES
    TIMEOUT 15
    ENVIRONMENT_MODIFICATION
      "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")

  if(IBMW_ENABLE_COMM_TRACE AND NOT IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE)
    add_library(ibmw_dds_extension_fault_test SHARED ${DDS_EXTENSION_SOURCES})
    ibmw_configure_dds_extension(
      ibmw_dds_extension_fault_test ibmw_dds_transport_fault_test)
    target_compile_definitions(ibmw_dds_extension_fault_test PRIVATE
      IBMW_COMM_TRACE_TEST_HOOKS=1)

    add_executable(ibmw_dds_managed_exact_fault_candidate
      dds_managed_exact_candidate.cc dds_bus.cc global.cc)
    ibmw_configure_managed_exact_candidate(
      ibmw_dds_managed_exact_fault_candidate ibmw_dds_extension_fault_test)
    target_compile_definitions(ibmw_dds_managed_exact_fault_candidate PRIVATE
      IBMW_MANAGED_EXACT_FAULT_CANDIDATE=1
      IBMW_COMM_TRACE_TEST_HOOKS=1)

    add_test(NAME ibmw_dds_managed_exact_candidate
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_managed_exact_candidate.py
        --candidate $<TARGET_FILE:ibmw_dds_managed_exact_candidate>
        --extension $<TARGET_FILE:ibmw_dds_extension>
        --fault-candidate $<TARGET_FILE:ibmw_dds_managed_exact_fault_candidate>
        --fault-extension $<TARGET_FILE:ibmw_dds_extension_fault_test>
        --production-query-test $<TARGET_FILE:ibmw_dds_extension_production_abi_test>
        --source-root ${CMAKE_SOURCE_DIR})
    set_tests_properties(ibmw_dds_managed_exact_candidate PROPERTIES
      TIMEOUT 600
      ENVIRONMENT
        "CTEST_BINARY_DIRECTORY=${CMAKE_BINARY_DIR}"
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:ibmw_dds_extension>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")

    if(IBMW_INSTALL)
      set(_ibmw_managed_exact_collector_prefix
          "${CMAKE_BINARY_DIR}/managed_exact_candidate_collector_install")
      add_test(NAME ibmw_dds_managed_exact_candidate_install_collector
        COMMAND ${CMAKE_COMMAND}
          -DBUILD_DIR=${CMAKE_BINARY_DIR}
          -DINSTALL_PREFIX=${_ibmw_managed_exact_collector_prefix}
          -DCOMPONENT=mw_perf_collect
          -P ${CMAKE_CURRENT_SOURCE_DIR}/install_clean_component.cmake)
      set_tests_properties(ibmw_dds_managed_exact_candidate_install_collector PROPERTIES
        FIXTURES_SETUP ibmw_dds_managed_exact_candidate_installed_collector
        TIMEOUT 60)

      add_test(NAME ibmw_dds_managed_exact_candidate_installed_collector
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/test_managed_exact_candidate.py
          --candidate $<TARGET_FILE:ibmw_dds_managed_exact_candidate>
          --extension $<TARGET_FILE:ibmw_dds_extension>
          --fault-candidate $<TARGET_FILE:ibmw_dds_managed_exact_fault_candidate>
          --fault-extension $<TARGET_FILE:ibmw_dds_extension_fault_test>
          --production-query-test $<TARGET_FILE:ibmw_dds_extension_production_abi_test>
          --source-root ${CMAKE_SOURCE_DIR}
          --collector-prefix ${_ibmw_managed_exact_collector_prefix})
      set_tests_properties(ibmw_dds_managed_exact_candidate_installed_collector PROPERTIES
        FIXTURES_REQUIRED ibmw_dds_managed_exact_candidate_installed_collector
        TIMEOUT 600
        ENVIRONMENT
          "CTEST_BINARY_DIRECTORY=${CMAKE_BINARY_DIR}"
        ENVIRONMENT_MODIFICATION
          "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:ibmw_dds_extension>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
      unset(_ibmw_managed_exact_collector_prefix)
    endif()

    add_test(NAME ibmw_dds_overlay_cross_dso
      COMMAND $<TARGET_FILE:ibmw_dds_managed_exact_candidate>
        --verify-overlay-dso)
    set_tests_properties(ibmw_dds_overlay_cross_dso PROPERTIES
      TIMEOUT 30
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:ibmw_dds_extension>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")

    if(CMAKE_READELF)
      add_test(NAME ibmw_dds_transport_rtti_symbols
        COMMAND ${CMAKE_COMMAND}
          -DREADELF=${CMAKE_READELF}
          "-DARTIFACTS=$<TARGET_FILE:ibmw_dds_managed_exact_candidate>;$<TARGET_FILE:ibmw_dds_extension>"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_transport_rtti_symbols.cmake)
      add_test(NAME ibmw_dds_transport_rtti_verifier_negative_cases
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/test_transport_rtti_verifier.py
          --cmake ${CMAKE_COMMAND}
          --verifier ${CMAKE_CURRENT_SOURCE_DIR}/verify_transport_rtti_symbols.cmake)
    endif()

    add_executable(ibmw_dds_extension_production_abi_test
      dds_extension_production_abi_test.cc)
    target_include_directories(ibmw_dds_extension_production_abi_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(ibmw_dds_extension_production_abi_test PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(ibmw_dds_managed_exact_candidate
      ibmw_dds_extension_production_abi_test)
    add_test(NAME ibmw_dds_extension_production_abi_test
      COMMAND $<TARGET_FILE:ibmw_dds_extension_production_abi_test>
        $<TARGET_FILE:ibmw_dds_extension>)
    set_tests_properties(ibmw_dds_extension_production_abi_test PROPERTIES
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
    add_test(NAME ibmw_dds_extension_production_dynamic_symbols
      COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM}
        -DLIB=$<TARGET_FILE:ibmw_dds_extension>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_no_fault_hook_symbols.cmake)

    add_executable(ibmw_dds_comm_trace_test
      dds_comm_trace_test.cc
      dds_comm_trace.cc)
    target_compile_definitions(ibmw_dds_comm_trace_test PRIVATE IBMW_ENABLE_COMM_TRACE=1)
    target_include_directories(ibmw_dds_comm_trace_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/..
      ${CMAKE_SOURCE_DIR}/src/core)
    target_link_libraries(ibmw_dds_comm_trace_test PRIVATE
      GTest::gtest_main
      OpenSSL::Crypto
      ${IBMW_FASTCDR_PROVIDER_TARGET}
      ${IBMW_FASTDDS_PROVIDER_TARGET})
    set_property(TARGET ibmw_dds_comm_trace_test PROPERTY
      BUILD_RPATH
        "$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
    add_test(NAME ibmw_dds_comm_trace_test
             COMMAND $<TARGET_FILE:ibmw_dds_comm_trace_test>)
    set_tests_properties(ibmw_dds_comm_trace_test PROPERTIES
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
  else()
    add_executable(ibmw_dds_trace_off_business_roundtrip
      dds_managed_exact_candidate.cc dds_bus.cc global.cc)
    target_compile_definitions(ibmw_dds_trace_off_business_roundtrip PRIVATE
      IBMW_TRACE_OFF_BUSINESS_CANDIDATE=1)
    target_include_directories(ibmw_dds_trace_off_business_roundtrip PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/..
      ${CMAKE_SOURCE_DIR}/src/core)
    target_link_libraries(ibmw_dds_trace_off_business_roundtrip PRIVATE
      ibmw_dds_extension
      dds_ext_proto
      ibmw::extensions::dds_types
      ibmw::core::engine
      ibmw::api::cpp
      ibmw::base
      yaml-cpp
      ${IBMW_FASTCDR_PROVIDER_TARGET}
      ${IBMW_FASTDDS_PROVIDER_TARGET})
    set_property(TARGET ibmw_dds_trace_off_business_roundtrip PROPERTY BUILD_RPATH
      "$<TARGET_FILE_DIR:ibmw_dds_extension>;$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")
    add_test(NAME ibmw_dds_trace_off_business_roundtrip
      COMMAND $<TARGET_FILE:ibmw_dds_trace_off_business_roundtrip>)
    set_tests_properties(ibmw_dds_trace_off_business_roundtrip PROPERTIES
      TIMEOUT 30
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:ibmw_dds_extension>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTCDR_PROVIDER_TARGET}>;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:${IBMW_FASTDDS_PROVIDER_TARGET}>")

    add_test(
      NAME ibmw_dds_comm_trace_compile_out_test
      COMMAND ${CMAKE_COMMAND}
        -DNM=${CMAKE_NM}
        -DLIB=$<TARGET_FILE:ibmw_dds_extension>
        -DEXE=$<TARGET_FILE:ibmw_dds_trace_off_business_roundtrip>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/verify_no_comm_trace_symbols.cmake)
  endif()
endif()
