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
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()

  option(IBMW_BUILD_TESTS "Build self-developed component tests." ON)
  option(IBMW_SELFDEV_SHM "Build selfdev shm (zero-copy shared memory transport)." ON)
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
            ARCHIVE DESTINATION lib)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include)
  endif()
endfunction()

# Components developed in parallel (crash_handler, concurrent, coro) are
# added by their own branches; shm only needs the helpers above.
if(IBMW_SELFDEV_SHM)
  add_subdirectory(shm)
endif()
