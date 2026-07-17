# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_FOONATHAN_MEMORY_SOURCE_URL
    "https://github.com/foonathan/memory/archive/refs/tags/v0.7-3.tar.gz"
    CACHE STRING "foonathan_memory source archive URL for IBMW source-prefix builds.")
set(IBMW_FOONATHAN_MEMORY_SOURCE_DIR
    ""
    CACHE PATH "Optional local foonathan_memory source tree for IBMW source-prefix builds.")

set(_ibmw_foonathan_memory_cmake_args
    -DFOONATHAN_MEMORY_BUILD_EXAMPLES=OFF
    -DFOONATHAN_MEMORY_BUILD_TESTS=OFF
    -DFOONATHAN_MEMORY_BUILD_TOOLS=OFF)

if(CMAKE_CROSSCOMPILING AND DEFINED OHOS_PLATFORM AND NOT "${OHOS_PLATFORM}" STREQUAL "")
  list(APPEND _ibmw_foonathan_memory_cmake_args
       -DCMAKE_CXX_FLAGS=-Wno-unused-command-line-argument)
endif()

ibmw_add_source_prefix_external_project(ibmw_source_foonathan_memory
  URL "${IBMW_FOONATHAN_MEMORY_SOURCE_URL}"
  SOURCE_DIR "${IBMW_FOONATHAN_MEMORY_SOURCE_DIR}"
  CMAKE_ARGS
    ${_ibmw_foonathan_memory_cmake_args})
