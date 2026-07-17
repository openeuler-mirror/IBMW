# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DdsIdlRpcGenCode.cmake - Generate C++ RPC wrapper code from DDS IDL service definitions
#
# This module provides functions to generate C++ RPC service/proxy classes
# from YAML service definition files that reference DDS IDL message types.
#
# The generated code uses ibmw::dds::GetDdsMessageTypeSupport<>() for
# CDR serialization and produces service classes (Sync, Async, Co),
# proxy classes (Sync, Async, Future, Co), CoClient, and CoServer.
#
# Usage:
#   add_dds_idl_rpc_gencode_target(
#     TARGET_NAME <target>
#     SERVICE_FILE <path/to/ExampleService.yaml>
#     GENCODE_PATH <output_directory>
#     IDL_FILE_STEM <ExampleRpc>
#     [DEP_IDL_TARGETS <dep1> [dep2 ...]]
#   )

# Generate C++ RPC wrapper code from a DDS IDL service definition YAML file
#
# Arguments:
#   TARGET_NAME     - Name of the CMake target to create
#   SERVICE_FILE    - Path to the YAML service definition file
#   GENCODE_PATH    - Output directory for generated code
#   IDL_FILE_STEM   - The IDL file stem (e.g., "ExampleRpc" for ExampleRpc.idl)
#   DEP_IDL_TARGETS - CMake targets for IDL gencode dependencies (optional)
#
function(add_dds_idl_rpc_gencode_target)
  cmake_parse_arguments(ARG "" "TARGET_NAME;SERVICE_FILE;GENCODE_PATH;IDL_FILE_STEM" "DEP_IDL_TARGETS" ${ARGN})

  # Ensure Python3::Interpreter is available (may not exist in consumer projects)
  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
  endif()

  if(NOT EXISTS ${ARG_GENCODE_PATH})
    file(MAKE_DIRECTORY ${ARG_GENCODE_PATH})
  endif()

  # Get the tool path from global property
  get_property(DDS_IDL_RPC_GEN_CODE_TOOL_PATH GLOBAL PROPERTY DDS_IDL_RPC_GEN_CODE_TOOL_PATH_PROPERTY)

  if(NOT DDS_IDL_RPC_GEN_CODE_TOOL_PATH)
    message(FATAL_ERROR "DDS_IDL_RPC_GEN_CODE_TOOL_PATH_PROPERTY not set. "
                        "Make sure dds_idl_gen_rpc tool is added before calling this function.")
  endif()

  # Determine output file names
  set(GEN_SRC "${ARG_GENCODE_PATH}/${ARG_IDL_FILE_STEM}.ibmw_rpc.dds.cc")
  set(GEN_HDR "${ARG_GENCODE_PATH}/${ARG_IDL_FILE_STEM}.ibmw_rpc.dds.h")

  # Build IDL gencode path arguments for include resolution
  set(IDL_GENCODE_PATHS)
  foreach(DEP ${ARG_DEP_IDL_TARGETS})
    get_target_property(_dep_gencode_path ${DEP} IDL_GENCODE_PATH)
    if(_dep_gencode_path)
      list(APPEND IDL_GENCODE_PATHS ${_dep_gencode_path})
    endif()
  endforeach()

  # Build --idl_gencode_path argument if we have any
  set(IDL_GENCODE_ARG)
  if(IDL_GENCODE_PATHS)
    list(GET IDL_GENCODE_PATHS 0 _first_path)
    set(IDL_GENCODE_ARG "--idl_gencode_path=${_first_path}")
  endif()

  # Custom command to run the Python code generator
  add_custom_command(
    OUTPUT ${GEN_SRC} ${GEN_HDR}
    COMMAND Python3::Interpreter ARGS
      ${DDS_IDL_RPC_GEN_CODE_TOOL_PATH}/dds_idl_gen_rpc.py
      --service_file=${ARG_SERVICE_FILE}
      --output_path=${ARG_GENCODE_PATH}
      ${IDL_GENCODE_ARG}
    DEPENDS
      ${ARG_SERVICE_FILE}
      ${DDS_IDL_RPC_GEN_CODE_TOOL_PATH}/dds_idl_gen_rpc.py
    COMMENT "Generating ibmw DDS RPC code for ${ARG_IDL_FILE_STEM}"
    VERBATIM
  )

  # Create a static library target with the generated code
  add_library(${ARG_TARGET_NAME} STATIC)
  set_target_properties(${ARG_TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

  target_sources(${ARG_TARGET_NAME} PRIVATE ${GEN_SRC})
  target_sources(${ARG_TARGET_NAME}
    PUBLIC FILE_SET HEADERS
    BASE_DIRS ${ARG_GENCODE_PATH}
    FILES ${GEN_HDR}
  )

  target_include_directories(${ARG_TARGET_NAME}
    PUBLIC
      $<BUILD_INTERFACE:${ARG_GENCODE_PATH}>
      $<INSTALL_INTERFACE:include/${ARG_TARGET_NAME}>
  )

  # Link against ibmw interfaces and IDL gencode dependencies.
  # NOTE: The dds_types dependency here is intentional and legitimate — generated
  # RPC code calls ibmw::dds::GetDdsMessageTypeSupport<>() (a real dds_extension
  # API). This is unlike DdsIdlGenCode.cmake's WRAP_IDLS path, which only needs
  # CDR-only headers and now links ibmw::schema::dds_cdr_helpers instead.
  target_link_libraries(${ARG_TARGET_NAME}
    PUBLIC
      ibmw::api::cpp
      ibmw::extensions::dds_types
      ${ARG_DEP_IDL_TARGETS}
  )

endfunction()
