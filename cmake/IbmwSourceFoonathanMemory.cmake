# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(IBMW_DDS_PROVIDER STREQUAL "SOURCE_FASTDDS2")
  set(_ibmw_foonathan_memory_source_url
      "https://codeload.github.com/foonathan/memory/tar.gz/19ab0759c7f053d88657c0eb86d879493f784d61")
  set(_ibmw_foonathan_memory_source_url_hash
      "SHA256=e117180f5dc8e335f746330e2c9f69a55ccf76e2d6a5bc7947d2d35afd91cd37")
else()
  set(_ibmw_foonathan_memory_source_url
      "https://github.com/foonathan/memory/archive/refs/tags/v0.7-3.tar.gz")
  set(_ibmw_foonathan_memory_source_url_hash
      "SHA256=4203d15db22a94a3978eeb1afb59a37d35c57c0f148733f0f1a53a6281cb74dd")
endif()

set(IBMW_FOONATHAN_MEMORY_SOURCE_URL
    "${_ibmw_foonathan_memory_source_url}"
    CACHE STRING "foonathan_memory source archive URL for IBMW source-prefix builds.")
set(IBMW_FOONATHAN_MEMORY_SOURCE_URL_HASH
    "${_ibmw_foonathan_memory_source_url_hash}"
    CACHE STRING "foonathan_memory source archive hash for IBMW source-prefix builds.")
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
  URL_HASH "${IBMW_FOONATHAN_MEMORY_SOURCE_URL_HASH}"
  SOURCE_DIR "${IBMW_FOONATHAN_MEMORY_SOURCE_DIR}"
  CMAKE_ARGS
    ${_ibmw_foonathan_memory_cmake_args})
