# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

cmake_minimum_required(VERSION 3.24)

project(ibmw LANGUAGES C CXX)

# Prevent variables from being reset by option
# This setting allows predefined variables to take precedence for FetchContent_MakeAvailable()
# See: https://cmake.org/cmake/help/latest/policy/CMP0077.html
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# Set cmake path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(CMakeDependentOption)
include(CheckCXXSourceCompiles)
include(GNUInstallDirs)

# ============================================================================
# Core options
# ============================================================================
option(IBMW_BUILD_TESTS "IBMW build tests." OFF)
option(IBMW_BUILD_RUNTIME "IBMW build runtime." ON)
option(IBMW_BUILD_CLI_TOOLS "IBMW build ibmw command line tools." OFF)
option(IBMW_BUILD_WITH_ROS2 "IBMW build with ROS2." OFF)
option(IBMW_BUILD_ROS2_TYPES "IBMW build ROS2 type support (rosidl build-time only, no rcl runtime dependency)." OFF)
option(IBMW_BUILD_PROTOCOLS "IBMW build protocols." ON)
option(IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE
  "Build the production DDS remote-managed exact-capture candidate." OFF)
option(IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE
  "Build and install the OHOS DDS remote-managed runtime-fault candidate." OFF)

# DDS selection is explicit. SYSTEM intentionally uses ordinary CMake package
# resolution, so a sourced ROS environment may affect package selection; it
# never adds hard-coded /opt/ros hints or implicitly switches provider classes.
set(IBMW_DDS_PROVIDER "SYSTEM" CACHE STRING
    "Fast DDS/Fast CDR provider: SYSTEM, ROS2_MINIMAL_SDK, SOURCE_FASTDDS2, or SOURCE_FASTDDS3")
set_property(CACHE IBMW_DDS_PROVIDER PROPERTY STRINGS SYSTEM ROS2_MINIMAL_SDK SOURCE_FASTDDS2 SOURCE_FASTDDS3)
set(IBMW_ROS2_MINIMAL_SDK_PREFIX "" CACHE PATH
    "Prefix containing the ros2_minimal_deps Humble Fast DDS 2.6.12/Fast CDR 1.0.29 SDK")
set(_IBMW_DDS_PROVIDERS SYSTEM ROS2_MINIMAL_SDK SOURCE_FASTDDS2 SOURCE_FASTDDS3)
if(NOT IBMW_DDS_PROVIDER IN_LIST _IBMW_DDS_PROVIDERS)
  message(FATAL_ERROR "Invalid IBMW_DDS_PROVIDER='${IBMW_DDS_PROVIDER}'. Allowed values: SYSTEM, ROS2_MINIMAL_SDK, SOURCE_FASTDDS2, SOURCE_FASTDDS3")
endif()
unset(_IBMW_DDS_PROVIDERS)

if(IBMW_DDS_PROVIDER MATCHES "^SOURCE_FASTDDS[23]$")
  set(IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD ON CACHE BOOL
      "Enable source-build ExternalProject targets for target-side third-party dependencies." FORCE)
elseif(IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
  message(FATAL_ERROR "IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD requires explicit IBMW_DDS_PROVIDER=SOURCE_FASTDDS2 or SOURCE_FASTDDS3")
endif()

# Auto-enable ROS2 types when full ROS2 support is requested
if(IBMW_BUILD_WITH_ROS2 AND NOT IBMW_BUILD_ROS2_TYPES)
  set(IBMW_BUILD_ROS2_TYPES ON)
endif()

include(IbmwCrossCompile)
include(IbmwThirdPartySourceBuild)
include(IbmwOhosRos2RosidlOverlay)

ibmw_ohos_ros2_rosidl_overlay_configure()
ibmw_cross_enable_target_ros2_prefixes()
ibmw_source_prefix_enable_package_discovery()
ibmw_configure_source_prefix_projects()

if(IBMW_THIRD_PARTY_SOURCE_BUILD_ONLY)
  message(STATUS "IBMW_THIRD_PARTY_SOURCE_BUILD_ONLY=ON: configured source-prefix ExternalProject targets only.")
  return()
endif()

option(IBMW_BUILD_WITH_WERROR "IBMW build with -Werror option." ON)
option(IBMW_INSTALL "Enable installation of IBMW." ON)

cmake_dependent_option(IBMW_BUILD_EXAMPLES "IBMW build examples." OFF "IBMW_BUILD_RUNTIME" OFF)

# Multi-transport end-to-end benchmark suite (src/samples/cpp/transport_benchmark).
# Compares iceoryx / self-developed shm / DDS (FastDDS) / native net_extension
# paths across the pub/sub sample pairs. Gated on a shared-memory extension
# (iceoryx or shm): each lane skips itself when its transport is off, so a
# shm-only Linux build gets the shm_* lanes without iceoryx (the OHOS cross build
# of the benchmark tree has only iox_loaned_ctx). The DDS extension should
# also be enabled to build the DDS lanes. Off by default to keep regular CI
# builds light.
cmake_dependent_option(IBMW_BUILD_BENCHMARKS "IBMW build multi-transport end-to-end benchmark suite." OFF "IBMW_BUILD_EXAMPLES;IBMW_BUILD_ICEORYX_EXTENSION OR IBMW_BUILD_SHM_EXTENSION" OFF)
if(CMAKE_CROSSCOMPILING)
  set(_IBMW_ROS2_SAMPLE_DEFAULT OFF)
else()
  set(_IBMW_ROS2_SAMPLE_DEFAULT ON)
endif()

cmake_dependent_option(IBMW_BUILD_ROS2_INTEROP_DEMO "IBMW build ROS2 interop demo." ${_IBMW_ROS2_SAMPLE_DEFAULT} "IBMW_BUILD_EXAMPLES;IBMW_BUILD_ROS2_TYPES" OFF)
cmake_dependent_option(IBMW_BUILD_ROS2_POINT_DEMO "IBMW build ROS2 point demo." ${_IBMW_ROS2_SAMPLE_DEFAULT} "IBMW_BUILD_EXAMPLES;IBMW_BUILD_ROS2_TYPES" OFF)
cmake_dependent_option(IBMW_BUILD_LOANED_MESSAGE_DEMO "IBMW build loaned message demo." ${_IBMW_ROS2_SAMPLE_DEFAULT} "IBMW_BUILD_EXAMPLES;IBMW_BUILD_ROS2_TYPES" OFF)
cmake_dependent_option(IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES "IBMW build ROS2 Action Fibonacci proof samples." ${_IBMW_ROS2_SAMPLE_DEFAULT} "IBMW_BUILD_ROS2_TYPES" OFF)
unset(_IBMW_ROS2_SAMPLE_DEFAULT)

# r8_validation deliberate-fail samples (mixed_api_fail trips
# IbmwR82SlotMixCheck<> static_assert; no_anchor_fail produces a fresh R1
# violation). Gated OFF so default CI keeps building clean. The Gate 9
# dual-build matrix opts in via -DIBMW_BUILD_R8_FAIL_SAMPLES=ON and asserts
# expected build failures.
cmake_dependent_option(IBMW_BUILD_R8_FAIL_SAMPLES "IBMW build r8_validation deliberate-fail samples (used by Gate 9 dual-build matrix)." OFF "IBMW_BUILD_EXAMPLES;IBMW_BUILD_ICEORYX_EXTENSION" OFF)

option(IBMW_USE_FMT_LIB "IBMW use fmt library." ON)
option(IBMW_ENABLE_DLOPEN_DEEPBIND "IBMW enable dlopen deepbind." ON)
option(IBMW_ENABLE_CORE_METRICS "Build Core Metrics V1 instrumentation." OFF)
option(IBMW_ENABLE_COMM_TRACE "Build internal P0a communication trace skeleton." OFF)
cmake_dependent_option(IBMW_ENABLE_REPORTER_V1
  "Build Reporter V1 ABI runtime (requires Core Metrics)." OFF
  "IBMW_ENABLE_CORE_METRICS" OFF)

option(IBMW_CONTEXT_DEFINE_GLOBAL_LOGGER_HANDLE "IBMW context define global logger handle." ON)

# ============================================================================
# Plugin options (all require runtime)
# ============================================================================
cmake_dependent_option(IBMW_BUILD_NET_EXTENSION "IBMW build net extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_MQTT_EXTENSION "IBMW build mqtt extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_ZENOH_EXTENSION "IBMW build zenoh extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_ICEORYX_EXTENSION "IBMW build iceoryx extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_SHM_EXTENSION "IBMW build self-developed zero-copy shm extension (iceoryx replacement, no RouDi)." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_RETAINED_LOAN_E2E "IBMW build retained-loan iceoryx E2E validator." OFF "IBMW_BUILD_RUNTIME;IBMW_BUILD_ICEORYX_EXTENSION" OFF)
option(IBMW_ICEORYX_USE_PACKAGE "Use a prebuilt target iceoryx package instead of FetchContent." OFF)
cmake_dependent_option(IBMW_BUILD_OPENTELEMETRY_EXTENSION "IBMW build opentelemetry extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_ECHO_EXTENSION "IBMW build echo extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_ROS2_EXTENSION "IBMW build ROS2 extension." OFF "IBMW_BUILD_RUNTIME;IBMW_BUILD_WITH_ROS2" OFF)
cmake_dependent_option(IBMW_BUILD_RECORD_PLAYBACK_EXTENSION "IBMW build record playback extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_TIME_MANIPULATOR_EXTENSION "IBMW build time manipulator extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_PARAMETER_EXTENSION "IBMW build parameter extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_LOG_CONTROL_EXTENSION "IBMW build log control extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_TOPIC_LOGGER_EXTENSION "IBMW build topic logger extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_GRPC_EXTENSION "IBMW build grpc extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_PROXY_EXTENSION "IBMW build proxy extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION "IBMW build service introspection extension." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_DDS_EXTENSION "IBMW build DDS extension (FastDDS)." OFF "IBMW_BUILD_RUNTIME" OFF)
cmake_dependent_option(IBMW_BUILD_DDS_XML_VALIDATION
  "Build the standalone DDS XML transport validation pair." OFF
  "IBMW_BUILD_RUNTIME;IBMW_BUILD_DDS_EXTENSION" OFF)

if(IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE OR IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE)
  foreach(_ibmw_candidate_requirement
      IBMW_BUILD_RUNTIME
      IBMW_ENABLE_COMM_TRACE
      IBMW_BUILD_DDS_EXTENSION
      IBMW_BUILD_PROTOCOLS)
    if(NOT ${_ibmw_candidate_requirement})
      message(FATAL_ERROR
        "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE requires ${_ibmw_candidate_requirement}=ON")
    endif()
  endforeach()
  unset(_ibmw_candidate_requirement)
endif()

option(IBMW_ZENOH_USE_PACKAGE "IBMW use an existing/prebuilt zenohc CMake package instead of building zenoh-c from source." OFF)

option(IBMW_EXECUTOR_USE_STDEXEC "IBMW use stdexec as executor impl. (Experimental)" OFF)

# ============================================================================
# Compiler settings
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set default build type as Release
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(READ "${PROJECT_SOURCE_DIR}/VERSION" IBMW_RUNTIME_VERSION)
string(STRIP "${IBMW_RUNTIME_VERSION}" IBMW_RUNTIME_VERSION)
message("IBMW Version : ${IBMW_RUNTIME_VERSION}")

# ============================================================================
# Standard library capability detection
# ============================================================================
check_cxx_source_compiles([[
  #include <source_location>

  int main() {
    const auto loc = std::source_location::current();
    return static_cast<int>(loc.line() == 0);
  }
]] IBMW_HAS_STD_SOURCE_LOCATION)

check_cxx_source_compiles([[
  #include <memory_resource>
  #include <string>

  int main() {
    std::pmr::monotonic_buffer_resource pool;
    std::pmr::string value(&pool);
    value = "ok";
    return static_cast<int>(value.empty());
  }
]] IBMW_HAS_STD_MEMORY_RESOURCE)

set(CMAKE_REQUIRED_QUIET TRUE)
check_cxx_source_compiles([[
  #include <memory>

  int main() {
    std::allocator<void>::rebind<char>::other allocator;
    char* value = allocator.allocate(1);
    allocator.deallocate(value, 1);
    return 0;
  }
]] IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING)
unset(CMAKE_REQUIRED_QUIET)

if(IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING)
  message(STATUS "IBMW ROS2 std_msgs/String support: enabled")
else()
  message(STATUS "IBMW ROS2 std_msgs/String support: disabled, allocator<void>::rebind unavailable")
endif()

# ============================================================================
# Generated config header
# ============================================================================
configure_file(
  "${PROJECT_SOURCE_DIR}/src/base/ibmw_config.h.in"
  "${PROJECT_BINARY_DIR}/generated/base/ibmw_config.h"
  @ONLY)

set(INSTALL_CONFIG_NAME ${PROJECT_NAME}-config)

# ============================================================================
# Master project configuration
# ============================================================================
if(NOT DEFINED IBMW_MASTER_PROJECT)
  set(IBMW_MASTER_PROJECT OFF)
  if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(IBMW_MASTER_PROJECT ON)
  endif()
endif()

if(IBMW_MASTER_PROJECT)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  if(NOT DEFINED FETCHCONTENT_BASE_DIR OR FETCHCONTENT_BASE_DIR STREQUAL "")
    set(FETCHCONTENT_BASE_DIR ${CMAKE_BINARY_DIR}/_deps)
  endif()

  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_C_VISIBILITY_INLINES_HIDDEN ON)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_INLINES_HIDDEN ON)

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(IBMW_BUILD_TESTS AND NOT ADDRESS_SANITIZER AND NOT THREAD_SANITIZER)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
    endif()
  endif()

  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  add_compile_options(/utf-8 /wd4819)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus")
endif()

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

# ============================================================================
# Include cmake modules & fetch dependencies
# ============================================================================
include(NamespaceTool)

# The benchmark payloads are CDR-serialized (bench_payload links dds_types).
if(IBMW_BUILD_PROTOCOLS OR IBMW_BUILD_DDS_EXTENSION OR
   IBMW_BUILD_ROS2_TYPES OR IBMW_BUILD_ICEORYX_EXTENSION OR
   IBMW_BUILD_BENCHMARKS)
  # Resolve the explicitly selected DDS provider before loading code-generation
  # helpers. Those helpers must never perform an independent provider search.
  include(GetFastDDS)
  include(DdsIdlGenCode)
  include(DdsIdlRpcGenCode)
  find_package(Python3 REQUIRED)
endif()

if(IBMW_BUILD_TESTS)
  include(GetGTest)
  enable_testing()

  add_test(NAME ibmw_fastdds_source_dependency_lock
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_fastdds_source_dependency_lock.sh")

  if(IBMW_MASTER_PROJECT AND IBMW_BUILD_RUNTIME AND
     CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_program(IBMW_TEST_BASH_EXECUTABLE NAMES bash)
    find_program(IBMW_TEST_LDD_EXECUTABLE NAMES ldd)
    find_program(IBMW_TEST_PYTHON3_EXECUTABLE NAMES python3)
    if(IBMW_TEST_BASH_EXECUTABLE AND IBMW_TEST_LDD_EXECUTABLE AND
       IBMW_TEST_PYTHON3_EXECUTABLE)
      add_test(
        NAME ibmw_installed_typesupport_package_test
        COMMAND ${IBMW_TEST_BASH_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/tests/test_installed_typesupport_package.sh)
      set(IBMW_INSTALLED_TYPESUPPORT_TEST_ENVIRONMENT
          "IBMW_INSTALLED_TYPESUPPORT_IBMW_BUILD=${CMAKE_BINARY_DIR}")
      if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT ADDRESS_SANITIZER
                                                     AND NOT THREAD_SANITIZER)
        list(APPEND IBMW_INSTALLED_TYPESUPPORT_TEST_ENVIRONMENT
             "IBMW_INSTALLED_TYPESUPPORT_LOADER_LINK_COVERAGE=1")
      endif()
      set_tests_properties(
        ibmw_installed_typesupport_package_test
        PROPERTIES ENVIRONMENT "${IBMW_INSTALLED_TYPESUPPORT_TEST_ENVIRONMENT}"
                   LABELS "integration;install;typesupport"
                   PROCESSORS 2
                   RUN_SERIAL TRUE
                   TIMEOUT 1800)
    else()
      message(
        WARNING
          "Skipping installed TypeSupport package test: bash, ldd, and python3 are required.")
    endif()
  endif()
endif()

if(IBMW_EXECUTOR_USE_STDEXEC)
  include(GetStdexec)
else()
  include(GetLibUnifex)
endif()

if(IBMW_USE_FMT_LIB)
  include(GetFmt)
endif()

if(IBMW_BUILD_CLI_TOOLS)
  find_package(Python3 REQUIRED)

  include(CheckPythonPackage)
  check_python_package(pyinstaller PYINSTALLER_FOUND)
  check_python_package(jinja2 JINJA2_FOUND)
  check_python_package(pyyaml PYYAML_FOUND)

  if(NOT PYINSTALLER_FOUND
     OR NOT JINJA2_FOUND
     OR NOT PYYAML_FOUND)
    set(IBMW_BUILD_CLI_TOOLS OFF)
    message(WARNING "Can not find pyinstaller, jinja2 or pyyaml in your python environment, will not compile ibmw_cli!")
    message(WARNING "Try to install pyinstaller, jinja2 and pyyaml by `pip3 install pyinstaller jinja2 pyyaml --upgrade`")
  endif()
endif()

# ROS2 support (conditional) — DEPRECATED
if(IBMW_BUILD_WITH_ROS2)
  message(WARNING
    "IBMW_BUILD_WITH_ROS2 is DEPRECATED and will be removed in a future release. "
    "IBMW now provides pure-DDS alternatives for all ROS2 interop scenarios:\n"
    "  - Pub/Sub: dds_extension with ros2_compatible_mode: true\n"
    "  - RPC/Service: dds_extension with type: dds service driver\n"
    "Use the 'full' CMake preset (cmake --preset full) for a build without rcl dependency."
  )
  # Fix cmake policy for using FindPythonInterp and FindPythonLibs (ros2)
  if(POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
  endif()

  include(GetJsonCpp)
  include(GetYamlCpp)
  ibmw_find_target_package(rclcpp REQUIRED FEATURE rclcpp-runtime)

  # Get rclcpp version info
  message(STATUS "rclcpp version: ${rclcpp_VERSION}")
  message(STATUS "rclcpp major version: ${rclcpp_VERSION_MAJOR}")

  if(rclcpp_VERSION_MAJOR EQUAL 16)
    message(STATUS "Building for ROS 2 Humble (rclcpp 16.x)")
  elseif(rclcpp_VERSION_MAJOR EQUAL 28)
    message(STATUS "Building for ROS 2 Jazzy (rclcpp 28.x)")
  else()
    message(FATAL_ERROR "Unsupported rclcpp major version: ${rclcpp_VERSION_MAJOR}. Only rclcpp 16.x (Humble) and 28.x (Jazzy) are supported.")
  endif()

  find_package(Python3 REQUIRED)
  include(GetEventsExecutor)
endif()

# Public TypeSupport adapters and generated DDS codecs use JSON.
include(GetNlohmannJson)

# Runtime dependencies
if(IBMW_BUILD_RUNTIME)
  include(GetAsio)
  include(GetGFlags)
  include(GetYamlCpp)
  include(GetTBB)
  include(GetBackward)
endif()

# FastDDS/FastCDR is required by both:
#   - schema (IBMW_BUILD_PROTOCOLS=ON): IDL gencode + WRAP_IDLS wrapper TUs
#     need FastCDR; the dds_extension target additionally needs FastDDS.
#   - runtime DDS extension (IBMW_BUILD_RUNTIME + IBMW_BUILD_DDS_EXTENSION).
# Pull it in whenever either is enabled, NOT only inside the runtime gate.
if(IBMW_BUILD_RUNTIME)
  if(IBMW_BUILD_NET_EXTENSION)
    include(GetBoost)
  endif()

  if(IBMW_BUILD_RECORD_PLAYBACK_EXTENSION)
    include(GetMcap)
  endif()

  if(IBMW_BUILD_ICEORYX_EXTENSION)
    # Try to find libacl
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|QNX")
      find_library(ACL_LIB acl)
      if(NOT ACL_LIB)
        message(WARNING "ICEORYX EXTENSION: libacl1-dev not found, please install the package by: sudo apt install libacl1-dev.")
        set(IBMW_BUILD_ICEORYX_EXTENSION OFF)
      endif()
    endif()

    if(IBMW_BUILD_ICEORYX_EXTENSION)
      if(NOT IBMW_ICEORYX_USE_PACKAGE)
        include(GetCppToml)
      endif()
      include(GetIceoryx)
    endif()
  endif()

  if(IBMW_BUILD_RETAINED_LOAN_E2E AND NOT IBMW_BUILD_ICEORYX_EXTENSION)
    message(FATAL_ERROR
      "IBMW_BUILD_RETAINED_LOAN_E2E requires IBMW_BUILD_ICEORYX_EXTENSION")
  endif()

  if(IBMW_BUILD_MQTT_EXTENSION)
    include(GetPahoMqttC)
  endif()

  if(IBMW_BUILD_ZENOH_EXTENSION)
    if(IBMW_ZENOH_USE_PACKAGE OR TARGET zenohc::lib)
      include(GetZenoh)
    else()
      # Find Rust compiler for zenoh-c source builds.
      execute_process(
        COMMAND rustc --version
        RESULT_VARIABLE rustc_result
        OUTPUT_VARIABLE rustc_output
        ERROR_QUIET)

      if(rustc_result EQUAL 0)
        message(STATUS "Rust compiler (rustc) found: ${rustc_output}")
        include(GetZenoh)
      else()
        message(
          WARNING
            "ZENOH_EXTENSION: Rust compiler (rustc) not found, will not compile zenoh extension. Provide a prebuilt zenohc package with -DIBMW_ZENOH_USE_PACKAGE=ON and CMAKE_PREFIX_PATH/zenohc_DIR, or install Rust for the source build.")
        set(IBMW_BUILD_ZENOH_EXTENSION OFF)
      endif()
    endif()
  endif()

  if(IBMW_BUILD_OPENTELEMETRY_EXTENSION)
    include(GetNlohmannJson)
    include(GetOpenTelemetryProto)
    include(GetOpenTelemetryCpp)
  endif()

  if(IBMW_BUILD_GRPC_EXTENSION)
    include(GetBoost)
    include(GetNghttp2)
  endif()

  if(IBMW_BUILD_ECHO_EXTENSION)
    include(GetJsonCpp)
  endif()

endif()

# ============================================================================
# Transport capability macros (ibmw::config + <ibmw/transport_features.h>)
# ============================================================================
# Must be included AFTER all IBMW_BUILD_*_EXTENSION options
# are resolved (cmake_dependent_option block + ROS2 / OpenTelemetry detection
# at lines 53-302) and BEFORE add_subdirectory(src) so ibmw::config is
# visible when src/api/cpp/CMakeLists.txt links it.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/IbmwTransportFlags.cmake)

# ============================================================================
# Add subdirectory
# ============================================================================
# Self-developed components must exist before src/ links them.
if(IBMW_BUILD_SHM_EXTENSION)
  # OHOS runs a Linux kernel (musl userland); CMake reports it as "OHOS".
  if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|OHOS)$")
    message(FATAL_ERROR "IBMW_BUILD_SHM_EXTENSION requires a Linux kernel (memfd/futex/epoll).")
  endif()
  set(IBMW_SELFDEV_SHM ON)
endif()
if(IBMW_SELFDEV_SHM)
  add_subdirectory(selfdev)
endif()

add_subdirectory(src)
add_subdirectory(tools)

# ============================================================================
# Installation
# ============================================================================
if(IBMW_INSTALL)
  include(CMakePackageConfigHelpers)

  # Install cmake helper modules (DdsIdlGenCode, Ros2IdlGenCode, etc.)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cmake
    DESTINATION ./
    FILES_MATCHING
    PATTERN "*.cmake"
    PATTERN "*.py"
    PATTERN "*.stg"
    PATTERN "ibmw-config.cmake.in" EXCLUDE)

  # Install the generated targets file (note: renamed from ibmw-config to
  # ibmw-targets so the hand-written config wrapper can call find_dependency()
  # before including targets)
  install(
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE ibmw-targets.cmake
    DESTINATION lib/cmake/${PROJECT_NAME})

  # Build-tree counterpart of install(EXPORT ...): emit ibmw-targets.cmake
  # into build-tree-config/lib/cmake/ibmw/ so external `find_package(ibmw)`
  # against a pure (un-installed) build tree can resolve our targets.
  # All 5 fetched third-party deps (asio / backward / yaml-cpp / TBB / unifex)
  # have been wired with their own export() calls (see Get*.cmake), so this top-level
  # export() can now succeed — they are all in their respective export sets.
  # See docs/contributing/testing.md for build-tree workflow validation.
  #
  # We skip build-tree export() when IBMW_BUILD_WITH_ROS2 is
  # enabled. The ROS 2 ament packages (example_ros2 / ros2_ext_proto) install
  # their *_ibmw_rpc_gencode targets into our INSTALL_CONFIG_NAME export set
  # but those targets PUBLIC-link to ament-managed rosidl typesupport targets
  # (e.g. example_ros2__rosidl_typesupport_cpp) which live in the ament
  # package's own export set, NOT ours. install(EXPORT ...) tolerates this
  # cross-package linkage by emitting find_dependency() at install-time
  # consumer config; export(EXPORT ...) (build-tree) rejects it outright.
  # The ROS 2 preset is itself DEPRECATED (see CMakePresets.json:87) — IBMW
  # provides pure-DDS alternatives — so the build-tree workflow is targeted
  # at the modern non-ROS2 flow only.
  if(NOT IBMW_BUILD_WITH_ROS2)
    export(
      EXPORT ${INSTALL_CONFIG_NAME}
      FILE "${CMAKE_CURRENT_BINARY_DIR}/build-tree-config/lib/cmake/${PROJECT_NAME}/ibmw-targets.cmake")
  endif()

  # ------------------------------------------------------------------------
  # Install-tree config: IBMW_HELPERS_DIR resolves to <prefix>/cmake
  # ------------------------------------------------------------------------
  set(IBMW_HELPERS_DIR cmake)
  set(IBMW_DDS_IDL_RPC_GEN_TOOL_PATH bin)
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ibmw-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/install/ibmw-config.cmake"
    INSTALL_DESTINATION lib/cmake/${PROJECT_NAME}
    PATH_VARS IBMW_HELPERS_DIR IBMW_DDS_IDL_RPC_GEN_TOOL_PATH)

  # Generate version file
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ibmw-config-version.cmake"
    VERSION ${IBMW_RUNTIME_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/install/ibmw-config.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/ibmw-config-version.cmake"
    DESTINATION lib/cmake/${PROJECT_NAME})

  # ------------------------------------------------------------------------
  # Build-tree convenience: pre-populate ibmw-config.cmake +
  # ibmw-config-version.cmake into build/build-tree-config/<lib>/cmake/ibmw/
  # at configure time so external `find_package(ibmw)` can locate them via
  # `-Dibmw_DIR=<build>/build-tree-config/lib/cmake/ibmw`.
  #
  # NOTE: the build-tree config is written to a SEPARATE directory (not
  # build/lib/cmake/ibmw/) so it does not collide with the install-tree
  # config produced by `cmake --install`. When the user runs
  # `cmake --install build --prefix build`, the install-tree config wins
  # at build/lib/cmake/ibmw/ with the relative `IBMW_HELPERS_DIR=cmake`
  # path; the build-tree config (absolute source path) lives separately.
  #
  # As of §6.2.2.22 (P0-2 final phase) the non-ROS2 build-tree workflow is
  # fully supported — the ibmw-targets.cmake is now also generated via export()
  # above, so the entire find_package(ibmw) chain works without install:
  #     cmake --preset full -B build
  #     cmake --build build -j$(nproc)
  #     # downstream:
  #     find_package(ibmw REQUIRED HINTS <build>/build-tree-config/lib/cmake/ibmw)
  # The install path remains supported and recommended for relocatable
  # packaging (the install-tree IBMW_HELPERS_DIR is relative to PACKAGE_PREFIX_DIR).
  # Keep this guarded exactly like the build-tree targets export above. When
  # IBMW_BUILD_WITH_ROS2 is enabled the export() call is intentionally skipped;
  # emitting ibmw-config.cmake without the matching ibmw-targets.cmake would
  # create a find_package(ibmw) prefix that is discoverable but unusable.
  if(NOT IBMW_BUILD_WITH_ROS2)
    set(IBMW_HELPERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    set(IBMW_DDS_IDL_RPC_GEN_TOOL_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tools/dds_idl_gen_rpc")
    configure_package_config_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ibmw-config.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/build-tree-config/lib/cmake/${PROJECT_NAME}/ibmw-config.cmake"
      INSTALL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/build-tree-config/lib/cmake/${PROJECT_NAME}"
      PATH_VARS IBMW_HELPERS_DIR IBMW_DDS_IDL_RPC_GEN_TOOL_PATH
      INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/build-tree-config")
    configure_file(
      "${CMAKE_CURRENT_BINARY_DIR}/ibmw-config-version.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/build-tree-config/lib/cmake/${PROJECT_NAME}/ibmw-config-version.cmake"
      COPYONLY)
  endif()
endif()

# ============================================================================
# Print all IBMW options
# ============================================================================
message("\n  IBMW CMake Options/Info:")

list(
  APPEND
  ibmw_vars
  IBMW_RUNTIME_VERSION
  IBMW_BUILD_TESTS
  IBMW_BUILD_RUNTIME
  IBMW_BUILD_PROTOCOLS
  IBMW_DDS_PROVIDER
  IBMW_ROS2_MINIMAL_SDK_PREFIX
  IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD
  IBMW_THIRD_PARTY_SOURCE_BUILD_ONLY
  IBMW_THIRD_PARTY_SOURCE_PREFIX
  IBMW_THIRD_PARTY_SOURCE_BASE_DIR
  IBMW_BUILD_CLI_TOOLS
  IBMW_BUILD_WITH_ROS2
  IBMW_BUILD_ROS2_TYPES
  IBMW_INSTALL
  IBMW_BUILD_EXAMPLES
  IBMW_USE_FMT_LIB
  IBMW_BUILD_NET_EXTENSION
  IBMW_BUILD_MQTT_EXTENSION
  IBMW_BUILD_ZENOH_EXTENSION
  IBMW_BUILD_ICEORYX_EXTENSION
  IBMW_BUILD_SHM_EXTENSION
  IBMW_BUILD_OPENTELEMETRY_EXTENSION
  IBMW_BUILD_ECHO_EXTENSION
  IBMW_BUILD_ROS2_EXTENSION
  IBMW_BUILD_RECORD_PLAYBACK_EXTENSION
  IBMW_BUILD_TIME_MANIPULATOR_EXTENSION
  IBMW_BUILD_PARAMETER_EXTENSION
  IBMW_BUILD_LOG_CONTROL_EXTENSION
  IBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION
  IBMW_BUILD_TOPIC_LOGGER_EXTENSION
  IBMW_BUILD_GRPC_EXTENSION
  IBMW_BUILD_PROXY_EXTENSION
  IBMW_BUILD_DDS_EXTENSION
  IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE
  IBMW_BUILD_DDS_MANAGED_EXACT_FAULT_CANDIDATE
  IBMW_EXECUTOR_USE_STDEXEC
  IBMW_ENABLE_DLOPEN_DEEPBIND
  IBMW_ENABLE_CORE_METRICS
  IBMW_ENABLE_COMM_TRACE
  IBMW_ENABLE_REPORTER_V1
  IBMW_BUILD_WITH_WERROR)

foreach(var ${ibmw_vars})
  string(LENGTH ${var} var_length)
  math(EXPR padding_length "40 - ${var_length}")
  if(padding_length GREATER 0)
    string(REPEAT "." ${padding_length} padding)
  else()
    set(padding "")
  endif()
  message("         ${var}${padding}: ${${var}}")
endforeach()
