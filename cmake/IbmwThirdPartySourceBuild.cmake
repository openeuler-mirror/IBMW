# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(ExternalProject)

option(IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD "Enable source-build ExternalProject targets for target-side third-party dependencies." OFF)
option(IBMW_THIRD_PARTY_SOURCE_BUILD_ONLY "Configure only the source-prefix ExternalProject targets, then stop before IBMW target discovery." OFF)

set(IBMW_THIRD_PARTY_SOURCE_PREFIX
    "${CMAKE_BINARY_DIR}/third_party_source_prefix"
    CACHE PATH "Install prefix populated by IBMW source-built third-party dependencies.")
set(IBMW_THIRD_PARTY_SOURCE_BASE_DIR
    "${CMAKE_BINARY_DIR}/third_party_source_build"
    CACHE PATH "ExternalProject work directory for IBMW source-built third-party dependencies.")
set(IBMW_THIRD_PARTY_SOURCE_BUILD_TYPE
    "Release"
    CACHE STRING "CMake build type used for source-built third-party dependencies.")
set_property(CACHE IBMW_THIRD_PARTY_SOURCE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

function(ibmw_source_prefix_enable_package_discovery)
  if(NOT IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
    return()
  endif()

  if(NOT IBMW_THIRD_PARTY_SOURCE_PREFIX)
    message(FATAL_ERROR "IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD requires IBMW_THIRD_PARTY_SOURCE_PREFIX.")
  endif()

  list(PREPEND CMAKE_PREFIX_PATH "${IBMW_THIRD_PARTY_SOURCE_PREFIX}")
  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)

  if(CMAKE_CROSSCOMPILING)
    list(PREPEND CMAKE_FIND_ROOT_PATH "${IBMW_THIRD_PARTY_SOURCE_PREFIX}")
    list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}" PARENT_SCOPE)

  endif()
endfunction()

function(ibmw_source_dep_common_cmake_args out_var)
  set(_args
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE=${IBMW_THIRD_PARTY_SOURCE_BUILD_TYPE}
    -DBUILD_SHARED_LIBS=ON
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF)

  if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND _args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
  endif()

  foreach(_var IN ITEMS
      CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
      CMAKE_SYSROOT
      CMAKE_C_COMPILER
      CMAKE_CXX_COMPILER
      CMAKE_AR
      CMAKE_RANLIB
      CMAKE_STRIP
      OHOS_SDK_NATIVE_ROOT
      OHOS_ARCH
      OHOS_PLATFORM
      OHOS_STL
      CMAKE_FIND_ROOT_PATH
      CMAKE_FIND_ROOT_PATH_MODE_PROGRAM
      CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
      CMAKE_FIND_ROOT_PATH_MODE_INCLUDE
      CMAKE_FIND_ROOT_PATH_MODE_PACKAGE)
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
      list(APPEND _args -D${_var}=${${_var}})
    endif()
  endforeach()

  set(${out_var} ${_args} PARENT_SCOPE)
endfunction()

function(ibmw_source_prefix_path_maybe_arg out_var var_name value)
  if(value)
    set(${out_var} -D${var_name}=${value} PARENT_SCOPE)
  else()
    set(${out_var} "" PARENT_SCOPE)
  endif()
endfunction()

function(ibmw_add_source_prefix_external_project target_name)
  set(_options)
  set(_one_value_args URL URL_HASH SOURCE_DIR)
  set(_multi_value_args DEPENDS CMAKE_ARGS PATCH_COMMAND)
  cmake_parse_arguments(_IBMW_DEP "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

  if(NOT IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
    return()
  endif()

  if(_IBMW_DEP_SOURCE_DIR)
    set(_download_args SOURCE_DIR "${_IBMW_DEP_SOURCE_DIR}")
  elseif(_IBMW_DEP_URL)
    set(_download_args URL "${_IBMW_DEP_URL}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    if(_IBMW_DEP_URL_HASH)
      list(APPEND _download_args URL_HASH "${_IBMW_DEP_URL_HASH}")
    endif()
  else()
    message(FATAL_ERROR "${target_name} requires URL or SOURCE_DIR for source-prefix build.")
  endif()

  if(_IBMW_DEP_PATCH_COMMAND)
    set(_patch_args PATCH_COMMAND ${_IBMW_DEP_PATCH_COMMAND})
  else()
    set(_patch_args "")
  endif()

  ibmw_source_dep_common_cmake_args(_common_args)

  ExternalProject_Add(${target_name}
    ${_download_args}
    ${_patch_args}
    PREFIX "${IBMW_THIRD_PARTY_SOURCE_BASE_DIR}/${target_name}"
    INSTALL_DIR "${IBMW_THIRD_PARTY_SOURCE_PREFIX}"
    CMAKE_ARGS ${_common_args} ${_IBMW_DEP_CMAKE_ARGS}
    BUILD_ALWAYS OFF
    DEPENDS ${_IBMW_DEP_DEPENDS})

  set_property(TARGET ${target_name} PROPERTY FOLDER third_party_source_build)
endfunction()

function(ibmw_add_source_prefix_header_project target_name)
  set(_options)
  set(_one_value_args URL URL_HASH SOURCE_DIR)
  set(_multi_value_args INSTALL_COMMAND)
  cmake_parse_arguments(_IBMW_DEP "${_options}" "${_one_value_args}" "${_multi_value_args}" ${ARGN})

  if(NOT IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
    return()
  endif()

  if(_IBMW_DEP_SOURCE_DIR)
    set(_download_args SOURCE_DIR "${_IBMW_DEP_SOURCE_DIR}")
  elseif(_IBMW_DEP_URL)
    set(_download_args URL "${_IBMW_DEP_URL}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    if(_IBMW_DEP_URL_HASH)
      list(APPEND _download_args URL_HASH "${_IBMW_DEP_URL_HASH}")
    endif()
  else()
    message(FATAL_ERROR "${target_name} requires URL or SOURCE_DIR for source-prefix build.")
  endif()

  ExternalProject_Add(${target_name}
    ${_download_args}
    PREFIX "${IBMW_THIRD_PARTY_SOURCE_BASE_DIR}/${target_name}"
    INSTALL_DIR "${IBMW_THIRD_PARTY_SOURCE_PREFIX}"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ${_IBMW_DEP_INSTALL_COMMAND}
    BUILD_ALWAYS OFF)

  set_property(TARGET ${target_name} PROPERTY FOLDER third_party_source_build)
endfunction()

function(ibmw_configure_source_prefix_projects)
  if(NOT IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
    return()
  endif()

  include(IbmwSourceTinyXML2)
  include(IbmwSourceFoonathanMemory)
  include(IbmwSourceAsio)
  include(IbmwSourceFastCDR)
  include(IbmwSourceFastDDS)
  ibmw_add_source_prefix_meta_target(
    ibmw_source_tinyxml2
    ibmw_source_foonathan_memory
    ibmw_source_asio
    ibmw_source_fastcdr
    ibmw_source_fastdds)
endfunction()

function(ibmw_add_source_prefix_meta_target)
  if(NOT IBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD)
    return()
  endif()

  if(NOT TARGET ibmw_third_party_source_prefix)
    add_custom_target(ibmw_third_party_source_prefix)
  endif()

  foreach(_target IN LISTS ARGN)
    if(TARGET ${_target})
      add_dependencies(ibmw_third_party_source_prefix ${_target})
    endif()
  endforeach()
endfunction()
