# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_ASIO_SOURCE_URL
    "https://github.com/chriskohlhoff/asio/archive/asio-1-30-2.tar.gz"
    CACHE STRING "Asio source archive URL for IBMW source-prefix builds.")
set(IBMW_ASIO_SOURCE_URL_HASH
    "SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8"
    CACHE STRING "Asio source archive hash for IBMW source-prefix builds.")
set(IBMW_ASIO_SOURCE_DIR
    ""
    CACHE PATH "Optional local Asio source tree for IBMW source-prefix builds.")

set(_ibmw_asio_install_script "${CMAKE_BINARY_DIR}/ibmw_source_asio_install.cmake")
file(WRITE "${_ibmw_asio_install_script}" [=[
cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED ASIO_SOURCE_DIR OR ASIO_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "ASIO_SOURCE_DIR is required")
endif()

if(NOT DEFINED ASIO_INSTALL_PREFIX OR ASIO_INSTALL_PREFIX STREQUAL "")
  message(FATAL_ERROR "ASIO_INSTALL_PREFIX is required")
endif()

set(_asio_include_dir "${ASIO_SOURCE_DIR}/asio/include")
if(NOT EXISTS "${_asio_include_dir}/asio.hpp")
  if(EXISTS "${ASIO_SOURCE_DIR}/include/asio.hpp")
    set(_asio_include_dir "${ASIO_SOURCE_DIR}/include")
  elseif(EXISTS "${ASIO_SOURCE_DIR}/include/asio/asio.hpp")
    set(_asio_include_dir "${ASIO_SOURCE_DIR}/include/asio")
  elseif(EXISTS "${ASIO_SOURCE_DIR}/asio/asio.hpp")
    set(_asio_include_dir "${ASIO_SOURCE_DIR}/asio")
  else()
    message(FATAL_ERROR "Could not find asio.hpp under ${ASIO_SOURCE_DIR}")
  endif()
endif()

file(INSTALL
  DESTINATION "${ASIO_INSTALL_PREFIX}/include"
  TYPE DIRECTORY
  FILES "${_asio_include_dir}/")

file(MAKE_DIRECTORY "${ASIO_INSTALL_PREFIX}/lib/cmake/asio")
file(WRITE "${ASIO_INSTALL_PREFIX}/lib/cmake/asio/asio-config.cmake" [==[
if(NOT TARGET asio::asio)
  add_library(asio::asio INTERFACE IMPORTED)
  get_filename_component(_asio_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  set_target_properties(asio::asio PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_asio_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "ASIO_STANDALONE;ASIO_NO_DEPRECATED")
endif()
]==])
]=])

ibmw_add_source_prefix_header_project(ibmw_source_asio
  URL "${IBMW_ASIO_SOURCE_URL}"
  URL_HASH "${IBMW_ASIO_SOURCE_URL_HASH}"
  SOURCE_DIR "${IBMW_ASIO_SOURCE_DIR}"
  INSTALL_COMMAND
    "${CMAKE_COMMAND}"
      -DASIO_SOURCE_DIR=<SOURCE_DIR>
      -DASIO_INSTALL_PREFIX=<INSTALL_DIR>
      -P "${_ibmw_asio_install_script}")
