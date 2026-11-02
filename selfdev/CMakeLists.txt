# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Self-developed replacements for third-party components.
# Each component lives in its own directory and does not depend on IBMW
# sources, so this directory can also be configured on its own:
#   cmake -S selfdev -B build-selfdev -DIBMW_BUILD_TESTS=ON \
#         -Dgoogletest_LOCAL_SOURCE=<path-to-googletest>

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.24)
  project(ibmw_selfdev LANGUAGES C CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()

  option(IBMW_BUILD_TESTS "Build self-developed component tests." ON)
  option(IBMW_SELFDEV_CRASH_HANDLER "Build selfdev crash_handler." ON)
  option(IBMW_SELFDEV_CONCURRENT "Build selfdev concurrent." ON)
  option(IBMW_EXECUTOR_USE_SELFDEV_CORO "Build selfdev coro." ON)
  # Benchmarks against oneTBB / libunifex / backward-cpp (see bench/).
  option(IBMW_SELFDEV_BUILD_BENCH "Build selfdev performance benchmarks." OFF)
  set(IBMW_BUILD_WITH_WERROR ON)
  set(IBMW_SELFDEV_STANDALONE ON)

  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../cmake)
  if(IBMW_BUILD_TESTS)
    enable_testing()
    include(GetGTest)
  endif()
endif()

find_package(Threads REQUIRED)

# Selfdev code is held to a stricter warning set than the IBMW default.
function(ibmw_selfdev_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow)
    if(IBMW_BUILD_WITH_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Treat googletest headers as system headers so selfdev warning flags do not
# fire inside them (e.g. clang -Wimplicit-int-float-conversion).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
  foreach(gt gtest gtest_main gmock gmock_main)
    if(TARGET ${gt})
      set_property(TARGET ${gt} PROPERTY SYSTEM ON)
    endif()
  endforeach()
endif()

function(ibmw_selfdev_add_test name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest GTest::gtest_main)
  ibmw_selfdev_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

function(ibmw_selfdev_install target)
  if(IBMW_INSTALL AND NOT IBMW_SELFDEV_STANDALONE)
    set_property(TARGET ${target} PROPERTY EXPORT_NAME ${ARGV1})
    install(TARGETS ${target}
            EXPORT ${INSTALL_CONFIG_NAME}
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin)
    # Headers only: editor/agent metadata next to them (hidden directories
    # such as .omc/) is not part of the delivery.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include
            FILES_MATCHING PATTERN "*.h"
            PATTERN ".*" EXCLUDE)
  endif()
endfunction()

# In-tree, crash_handler and concurrent are only used by the runtime;
# coro is used by ibmw::api::cpp and is needed even without it.
if(IBMW_SELFDEV_STANDALONE OR IBMW_BUILD_RUNTIME)
  set(_ibmw_selfdev_runtime ON)
else()
  set(_ibmw_selfdev_runtime OFF)
endif()

if(IBMW_SELFDEV_CRASH_HANDLER AND _ibmw_selfdev_runtime)
  add_subdirectory(crash_handler)
endif()

if(IBMW_SELFDEV_CONCURRENT AND _ibmw_selfdev_runtime)
  add_subdirectory(concurrent)
endif()

if(IBMW_EXECUTOR_USE_SELFDEV_CORO)
  add_subdirectory(coro)
endif()

# Standalone only: an IBMW build never configures the benchmarks.
if(IBMW_SELFDEV_STANDALONE AND IBMW_SELFDEV_BUILD_BENCH)
  add_subdirectory(bench)
endif()
