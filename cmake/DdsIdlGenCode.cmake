# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DdsIdlGenCode.cmake - Generate C++ code from DDS IDL files using fastddsgen
#
# This module provides functions to generate C++ type support code from DDS IDL files.
# It replaces the protobuf-based code generation (ProtobufGenCode.cmake).
#
# Generated files:
#   <TypeName>.hpp         - Type definition (always usable)
#   <TypeName>CdrAux.hpp   - CDR serialization aux header (only needs fastcdr)
#   <TypeName>CdrAux.ipp   - CDR serialization aux implementation (only needs fastcdr)
#   <TypeName>PubSubTypes.hpp/cxx - PubSub type support (needs FastDDS 3.x)
#
# Note: fastddsgen v4.x generates PubSubTypes that require FastDDS 3.x API.
# Since this build environment may have FastDDS 2.6.x (ROS2 Humble), we generate
# all files but only compile the CdrAux (which only depends on FastCDR).
# The PubSubTypes are generated but not compiled — they can be used when
# FastDDS 3.x is available.
#
# Usage:
#   add_dds_idl_gencode_target(
#     TARGET_NAME <target>
#     IDL_FILES <file1.idl> [file2.idl ...]
#     GENCODE_PATH <output_directory>
#     [INCLUDE_DIRS <dir1> [dir2 ...]]
#     [DEPENDENCIES <dep1> [dep2 ...]]
#   )

# Cache this module's directory at file scope so that functions defined below
# can locate helper scripts (e.g. fastcdr_v2_to_v1.py) regardless of where
# the calling CMakeLists.txt is located.  Inside a function, CMAKE_CURRENT_LIST_DIR
# resolves to the *caller's* directory, not this file's directory.
set(_DDS_IDL_GEN_CODE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Find fastddsgen tool
find_program(FASTDDSGEN_EXECUTABLE
  NAMES fastddsgen
  HINTS
    $ENV{FASTDDS_HOME}/bin
    $ENV{FASTRTPS_HOME}/bin
  DOC "Path to fastddsgen IDL compiler"
)

if(NOT FASTDDSGEN_EXECUTABLE)
  message(STATUS "fastddsgen not found. DDS IDL code generation will not be available.")
  message(STATUS "Install Fast-DDS-Gen from: https://github.com/eProsima/Fast-DDS-Gen")
else()
  message(STATUS "Found fastddsgen: ${FASTDDSGEN_EXECUTABLE}")
endif()

# Detect FastDDS version to decide whether to compile PubSubTypes
# FastDDS 3.x is required for the generated PubSubTypes; 2.x only supports CdrAux
set(IBMW_FASTDDS_V3_AVAILABLE FALSE)
if(fastdds_FOUND AND fastdds_VERSION AND fastdds_VERSION VERSION_GREATER_EQUAL 3
   AND IBMW_FASTDDS_PROVIDER_TARGET AND TARGET ${IBMW_FASTDDS_PROVIDER_TARGET})
  set(IBMW_FASTDDS_V3_AVAILABLE TRUE)
endif()

# Generate C++ code from DDS IDL files
#
# This function creates a library target containing the generated C++ code
# from the specified DDS IDL files.
#
# Arguments:
#   TARGET_NAME   - Name of the CMake target to create
#   IDL_FILES     - List of .idl source files
#   GENCODE_PATH  - Output directory for generated code
#   INCLUDE_DIRS  - Additional include directories for IDL compiler (optional)
#   DEPENDENCIES  - CMake targets that this target depends on (optional)
#
function(add_dds_idl_gencode_target)
  cmake_parse_arguments(ARG "" "TARGET_NAME;GENCODE_PATH" "IDL_FILES;INCLUDE_DIRS;DEPENDENCIES;UPSTREAM_GENCODE_PATHS;WRAP_IDLS" ${ARGN})

  # Select the FastCDR lane when the target is declared, after GetFastDDS.cmake
  # has had a chance to populate fastcdr_VERSION. Preserve the historical v1
  # compatibility transform when fastcdr_VERSION is unknown/empty or older than
  # 2.0; FastCDR 2.x+ consumes fastddsgen's native v2 CdrAux output as-is.
  set(IBMW_DDS_IDL_FASTCDR_V1_COMPAT TRUE)
  if(DEFINED fastcdr_VERSION AND fastcdr_VERSION AND NOT fastcdr_VERSION VERSION_LESS 2.0)
    set(IBMW_DDS_IDL_FASTCDR_V1_COMPAT FALSE)
  endif()
  if(IBMW_DDS_IDL_FASTCDR_V1_COMPAT)
    if(DEFINED fastcdr_VERSION AND fastcdr_VERSION)
      message(STATUS "DDS IDL CDR lane for ${ARG_TARGET_NAME}: FastCDR v1 compatibility transform (fastcdr_VERSION=${fastcdr_VERSION})")
    else()
      message(WARNING "DDS IDL CDR lane for ${ARG_TARGET_NAME}: FastCDR v1 compatibility transform (fastcdr_VERSION unknown)")
    endif()
  else()
    message(STATUS "DDS IDL CDR lane for ${ARG_TARGET_NAME}: native FastCDR v2+ generated code (fastcdr_VERSION=${fastcdr_VERSION})")
  endif()

  # WRAP_IDLS: opt-in list of IDL stem names (without .idl) that should get an
  # auto-generated CdrImpl.cc wrapper TU compiled into a STATIC archive. IDLs
  # not in this list are still generated (headers, .ipp) but no wrapper TU is
  # emitted for them, keeping the library effectively INTERFACE for those types.
  # This guards us from py-transformer limitations on complex IDLs (unions,
  # nested-struct vectors) until those patterns are supported.
  # If WRAP_IDLS is empty, the resulting library is INTERFACE (header-only).

  if(NOT FASTDDSGEN_EXECUTABLE)
    message(FATAL_ERROR "fastddsgen is required but not found. Cannot generate code for target ${ARG_TARGET_NAME}")
  endif()

  if(NOT EXISTS ${ARG_GENCODE_PATH})
    file(MAKE_DIRECTORY ${ARG_GENCODE_PATH})
  endif()

  # Determine the common IDL source directory for relative path computation.
  # All IDL files in a single target share the same parent directory.
  list(GET ARG_IDL_FILES 0 _first_idl)
  get_filename_component(_IDL_BASE_DIR ${_first_idl} DIRECTORY)

  # Build include path arguments using relative paths.
  # fastddsgen v4.x has a quirk where absolute -I paths cause generated C++
  # #includes to use the full path (minus leading /). Using relative paths
  # produces clean basename-only #includes like "Header.hpp".
  set(INCLUDE_ARGS)
  foreach(INC_DIR ${ARG_INCLUDE_DIRS})
    file(RELATIVE_PATH _rel_inc "${_IDL_BASE_DIR}" "${INC_DIR}")
    if(_rel_inc STREQUAL "")
      set(_rel_inc ".")
    endif()
    list(APPEND INCLUDE_ARGS "-I" "${_rel_inc}")
  endforeach()

  # Forward UPSTREAM_GENCODE_PATHS to the post-process script so cross-package
  # CdrAux.ipp dependencies can be injected (e.g. sensor's CompressedImageCdrAux.ipp
  # gains an `#include "HeaderCdrAux.ipp"` line resolving to the common gencode dir).
  set(UPSTREAM_DIR_ARGS)
  foreach(UP_DIR ${ARG_UPSTREAM_GENCODE_PATHS})
    list(APPEND UPSTREAM_DIR_ARGS "--upstream-dir" "${UP_DIR}")
  endforeach()

  set(GEN_SRCS)
  set(GEN_HDRS)
  # cdr_impl wrapper TUs auto-generated below (one per IDL); compiled into
  # the gencode static library so downstream consumers no longer need to hand-roll
  # their own per-sample compressed_image_cdr_impl.cc files.
  set(GEN_CDR_IMPL_TUS)
  # Track per-IDL CdrAux.ipp outputs across the loop so each subsequent IDL's
  # post-process (inject_dependent_ipp_includes) DEPENDS on previously-written
  # .ipp files. Without this serialization, parallel custom-command execution
  # (`make -j`) can run ExampleRpc's transform before ExampleCommon's
  # fastddsgen finishes writing ExampleCommonCdrAux.ipp — the inject step then
  # fails to discover the dependent .ipp and ExampleRpcCdrAux.ipp is missing
  # the `#include "ExampleCommonCdrAux.ipp"` line, breaking compilation of
  # downstream RPC types whose fields reference ExampleCommon types.
  # See docs/contributing/testing.md.
  set(_PREV_IDL_IPP_OUTPUTS)

  foreach(IDL_FILE ${ARG_IDL_FILES})
    get_filename_component(IDL_NAME ${IDL_FILE} NAME_WE)
    get_filename_component(IDL_BASENAME ${IDL_FILE} NAME)
    get_filename_component(IDL_DIR ${IDL_FILE} DIRECTORY)

    # fastddsgen generates these files per IDL:
    set(GEN_TYPE_HPP "${ARG_GENCODE_PATH}/${IDL_NAME}.hpp")
    set(GEN_PUBSUB_CXX "${ARG_GENCODE_PATH}/${IDL_NAME}PubSubTypes.cxx")
    set(GEN_PUBSUB_HPP "${ARG_GENCODE_PATH}/${IDL_NAME}PubSubTypes.hpp")
    set(GEN_CDR_IPP "${ARG_GENCODE_PATH}/${IDL_NAME}CdrAux.ipp")
    set(GEN_CDR_HPP "${ARG_GENCODE_PATH}/${IDL_NAME}CdrAux.hpp")
    # Auto-generated 2-line wrapper TU that #includes the .ipp once so the
    # inline FastCDR specializations get emitted into the static library.
    set(GEN_CDR_IMPL_TU "${ARG_GENCODE_PATH}/${IDL_NAME}CdrImpl.cc")

    # Per-IDL output list for this custom command
    set(CUR_IDL_OUTPUTS ${GEN_TYPE_HPP} ${GEN_PUBSUB_CXX} ${GEN_PUBSUB_HPP} ${GEN_CDR_IPP} ${GEN_CDR_HPP})

    # Accumulate headers and sources across all IDL files
    list(APPEND GEN_HDRS ${GEN_TYPE_HPP} ${GEN_CDR_HPP} ${GEN_CDR_IPP})

    # Only wrap IDLs explicitly opted-in via WRAP_IDLS. This avoids forcing
    # the py CDR transformer to handle every IDL pattern (unions, nested-struct
    # vectors, etc.) up-front; new types can be added to the list once their
    # wrapper TU compiles cleanly.
    set(_WRAP_THIS_IDL FALSE)
    if(IDL_NAME IN_LIST ARG_WRAP_IDLS)
      set(_WRAP_THIS_IDL TRUE)
      list(APPEND GEN_CDR_IMPL_TUS ${GEN_CDR_IMPL_TU})
    endif()

    # Only compile PubSubTypes if FastDDS 3.x is available
    if(IBMW_FASTDDS_V3_AVAILABLE)
      list(APPEND GEN_SRCS ${GEN_PUBSUB_CXX})
      list(APPEND GEN_HDRS ${GEN_PUBSUB_HPP})
    endif()

    set(_POSTPROCESS_COMMAND)
    set(_GENCODE_COMMENT "Running fastddsgen on ${IDL_BASENAME} (native FastCDR v2+)")
    if(IBMW_DDS_IDL_FASTCDR_V1_COMPAT)
      # Post-process: convert FastCDR v2 API calls to v1-compatible code.
      # fastddsgen v4.x generates v2-only CdrAux code, but older FastCDR lanes
      # (including unknown fastcdr_VERSION, for historical compatibility) need
      # the legacy transform. This script transforms member-function calls
      # (begin_serialize_type, end_serialize_type, deserialize_type, etc.)
      # into v1-compatible patterns. Also processes .hpp files to remove
      # v2-only includes like fixed_size_string.hpp, and PubSubTypes.cxx
      # to replace operator<</>> with explicit free-function serialize/deserialize.
      # The --upstream-dir args allow cross-package CdrAux.ipp injection so a
      # sensor-package wrapper TU can resolve common-package Header specializations.
      set(_POSTPROCESS_COMMAND
        COMMAND ${CMAKE_COMMAND} -E env python3
          ${_DDS_IDL_GEN_CODE_CMAKE_DIR}/fastcdr_v2_to_v1.py
          ${UPSTREAM_DIR_ARGS}
          ${GEN_CDR_IPP}
          ${GEN_TYPE_HPP}
          ${GEN_PUBSUB_CXX})
      set(_GENCODE_COMMENT "Running fastddsgen on ${IDL_BASENAME} (with v1 compat)")
    endif()

    add_custom_command(
      OUTPUT ${CUR_IDL_OUTPUTS}
      COMMAND ${FASTDDSGEN_EXECUTABLE}
        -d ${ARG_GENCODE_PATH}
        -flat-output-dir
        -no-typeobjectsupport
        -no-dependencies
        -typeros2
        -cs
        -replace
        ${INCLUDE_ARGS}
        ${IDL_BASENAME}
      ${_POSTPROCESS_COMMAND}
      # DEPENDS the IDL source plus the .ipp outputs of all previously processed
      # IDLs in this target. The latter forces serial execution under `make -j`
      # so inject_dependent_ipp_includes can see same-target dependent .ipp
      # files (e.g. ExampleRpc -> ExampleCommon). See §6.2.2.18.
      DEPENDS ${IDL_FILE} ${_PREV_IDL_IPP_OUTPUTS}
      WORKING_DIRECTORY ${IDL_DIR}
      COMMENT "${_GENCODE_COMMENT}"
      VERBATIM
    )

    # Append this IDL's .ipp to the running serialization chain.
    list(APPEND _PREV_IDL_IPP_OUTPUTS ${GEN_CDR_IPP})

    # Synthesize a tiny per-IDL wrapper TU at configure time only for opt-in IDLs.
    # Compiling this TU into the gencode STATIC library emits the inline FastCDR
    # specializations exactly once per process, so downstream samples need only
    # link the gencode lib — no hand-rolled <type>_cdr_impl.cc + per-sample STATIC
    # wrapper required. file(WRITE) at configure time avoids Makefile/Ninja issues
    # with multi-line `cmake -E echo` shell-redirection commands.
    if(_WRAP_THIS_IDL)
      file(WRITE ${GEN_CDR_IMPL_TU}
"// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Auto-generated by add_dds_idl_gencode_target. DO NOT EDIT.
// Compiling this single TU emits the inline FastCDR serialize/deserialize
// specializations for ${IDL_NAME} into the gencode static library.

#include \"${IDL_NAME}CdrAux.ipp\"
")
    endif()
  endforeach()

  # Create the library target
  if(GEN_SRCS)
    # FastDDS 3.x available: compile PubSubTypes (+ any opted-in CDR impl wrappers)
    add_library(${ARG_TARGET_NAME} STATIC ${GEN_SRCS} ${GEN_CDR_IMPL_TUS})
    target_include_directories(${ARG_TARGET_NAME}
      PUBLIC
        $<BUILD_INTERFACE:${ARG_GENCODE_PATH}>
        $<INSTALL_INTERFACE:include/${ARG_TARGET_NAME}>
    )
    foreach(UP_DIR ${ARG_UPSTREAM_GENCODE_PATHS})
      target_include_directories(${ARG_TARGET_NAME} PRIVATE $<BUILD_INTERFACE:${UP_DIR}>)
    endforeach()
    target_sources(${ARG_TARGET_NAME}
      PUBLIC FILE_SET HEADERS
      BASE_DIRS ${ARG_GENCODE_PATH}
      FILES ${GEN_HDRS}
    )
    target_link_libraries(${ARG_TARGET_NAME} PUBLIC fastcdr fastdds)
    if(IBMW_DDS_IDL_FASTCDR_V1_COMPAT)
      # Transformed CdrAux.ipp helpers include dds_types/util/fastcdr_v1_compat.h.
      # Use the schema-layer helper target rather than the DDS extension target
      # so protocol/schema builds do not acquire a reverse dependency on
      # IBMW_BUILD_DDS_EXTENSION.
      target_link_libraries(${ARG_TARGET_NAME} PUBLIC ibmw::schema::dds_cdr_helpers)
    endif()
  elseif(GEN_CDR_IMPL_TUS)
    # FastDDS 2.x with at least one opted-in IDL: compile the auto-generated CDR
    # impl wrappers into a STATIC library so downstream consumers can drop their
    # hand-written cdr_impl.cc files.
    add_library(${ARG_TARGET_NAME} STATIC ${GEN_CDR_IMPL_TUS})
    target_include_directories(${ARG_TARGET_NAME}
      PUBLIC
        $<BUILD_INTERFACE:${ARG_GENCODE_PATH}>
        $<INSTALL_INTERFACE:include/${ARG_TARGET_NAME}>
    )
    foreach(UP_DIR ${ARG_UPSTREAM_GENCODE_PATHS})
      target_include_directories(${ARG_TARGET_NAME} PRIVATE $<BUILD_INTERFACE:${UP_DIR}>)
    endforeach()
    target_sources(${ARG_TARGET_NAME}
      PUBLIC FILE_SET HEADERS
      BASE_DIRS ${ARG_GENCODE_PATH}
      FILES ${GEN_HDRS}
    )
    target_link_libraries(${ARG_TARGET_NAME} PUBLIC fastcdr)
    if(IBMW_DDS_IDL_FASTCDR_V1_COMPAT)
      # The wrapper TU pulls in dds_types/util/fastcdr_v1_compat.h via the
      # transformed .ipp. Link the schema-layer CDR-only helpers target (NOT the
      # dds_extension dds_types target) to avoid a reverse dependency: schema
      # must not require IBMW_BUILD_DDS_EXTENSION. See src/schema/CMakeLists.txt
      # for the helpers target definition.
      target_link_libraries(${ARG_TARGET_NAME} PUBLIC ibmw::schema::dds_cdr_helpers)
    endif()
  else()
    # FastDDS 2.x with no opted-in IDLs: header-only INTERFACE library. Consumers
    # that need CDR symbols for these types must still hand-roll their own
    # per-sample cdr_impl.cc until those IDLs are added to WRAP_IDLS.
    add_library(${ARG_TARGET_NAME} INTERFACE)
    target_include_directories(${ARG_TARGET_NAME}
      INTERFACE
        $<BUILD_INTERFACE:${ARG_GENCODE_PATH}>
        $<INSTALL_INTERFACE:include/${ARG_TARGET_NAME}>
    )
    target_sources(${ARG_TARGET_NAME}
      INTERFACE FILE_SET HEADERS
      BASE_DIRS ${ARG_GENCODE_PATH}
      FILES ${GEN_HDRS}
    )
    target_link_libraries(${ARG_TARGET_NAME} INTERFACE fastcdr)
    # Custom target to drive code generation when no compilable sources exist.
    add_custom_target(${ARG_TARGET_NAME}_codegen ALL DEPENDS ${GEN_HDRS})
    add_dependencies(${ARG_TARGET_NAME} ${ARG_TARGET_NAME}_codegen)
  endif()

  # Link dependencies. Use INTERFACE keyword for INTERFACE lib, PUBLIC otherwise.
  if(TARGET ${ARG_TARGET_NAME})
    get_target_property(_lib_type ${ARG_TARGET_NAME} TYPE)
    if(_lib_type STREQUAL "INTERFACE_LIBRARY")
      set(_link_keyword INTERFACE)
    else()
      set(_link_keyword PUBLIC)
    endif()
    foreach(DEP ${ARG_DEPENDENCIES})
      target_link_libraries(${ARG_TARGET_NAME} ${_link_keyword} ${DEP})
    endforeach()
  endif()

  # Set properties for downstream consumers (skip for INTERFACE libs which can't
  # carry custom properties via set_target_properties without INTERFACE_ prefix).
  if(NOT _lib_type STREQUAL "INTERFACE_LIBRARY")
    set_target_properties(${ARG_TARGET_NAME} PROPERTIES
      IDL_GENCODE_PATH ${ARG_GENCODE_PATH}
    )
  endif()

endfunction()
