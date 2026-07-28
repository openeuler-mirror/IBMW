# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# IbmwHelpers.cmake - High-level convenience macros for IBMW external projects
#
# This module is automatically included when you call find_package(ibmw).
# It provides one-line macros to replace repetitive CMake boilerplate.
#
# Provided macros:
#   ibmw_add_module()        - Create a module library (STATIC, PIC, linked to ibmw::api::cpp)
#   ibmw_add_pkg()           - Create a pkg-mode shared library plugin
#   ibmw_add_app()           - Create an app-mode standalone executable
#   ibmw_add_dds_messages()  - Compile CDR impl + generate type bridge for DDS IDL types
#   ibmw_add_dds_rpc()       - Convenience wrapper for DDS RPC gencode targets
#   ibmw_add_json_types()    - (Optional) Document JSON type dependencies
#   ibmw_add_rosidl_interface_target() - Wrap generated rosidl package targets

# --------------------------------------------------------------------------
# ibmw_add_module(<name>
#   SRCS <source files...>
#   [DEPS <link dependencies...>]
#   [INCLUDE_DIR <base include directory>]
# )
#
# Creates a STATIC library with POSITION_INDEPENDENT_CODE ON, linked to
# ibmw::api::cpp, with a default PUBLIC include directory set to the
# project source tree.
#
# Example:
#   ibmw_add_module(my_publisher
#     SRCS src/my_publisher/my_publisher.cc
#     DEPS ibmw::extensions::dds_types my_msgs_cdr)
#
# Equivalent to:
#   add_library(my_publisher STATIC src/my_publisher/my_publisher.cc)
#   target_include_directories(my_publisher PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
#   set_target_properties(my_publisher PROPERTIES POSITION_INDEPENDENT_CODE ON)
#   target_link_libraries(my_publisher PUBLIC ibmw::api::cpp ibmw::extensions::dds_types my_msgs_cdr)
# --------------------------------------------------------------------------
function(ibmw_add_module TARGET_NAME)
  cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SRCS;DEPS" ${ARGN})

  if(NOT ARG_SRCS)
    message(FATAL_ERROR "ibmw_add_module(${TARGET_NAME}): SRCS is required")
  endif()

  # Default include directory: src/ under the project source root
  if(NOT ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()

  add_library(${TARGET_NAME} STATIC ${ARG_SRCS})

  target_include_directories(${TARGET_NAME}
    PUBLIC $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)

  set_target_properties(${TARGET_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

  target_link_libraries(${TARGET_NAME}
    PUBLIC ibmw::api::cpp ${ARG_DEPS})
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_rosidl_interface_target(<name>
#   PACKAGE_NAME <ros2_interface_package>
#   [GENERATOR_CPP_TARGET <target>]
#   [FASTRTPS_CPP_TARGET <target>]
#   [FASTRTPS_C_TARGET <target>]
# )
#
# Creates an INTERFACE target that forwards the canonical rosidl-generated C++
# message target and FastRTPS C/C++ typesupport targets from a ROS2 interface
# package generated or referenced by ibmw-gen import. This is intentionally a
# CMake/link glue helper only: it does not generate runtime modules and does
# not replace ROS2's rosidl_typesupport_fastrtps_* compatibility contract.
#
# Example:
#   ibmw_add_rosidl_interface_target(my_msgs_ibmw_iface
#     PACKAGE_NAME my_msgs)
#
# By default the helper derives canonical target names:
#   my_msgs::my_msgs__rosidl_generator_cpp
#   my_msgs::my_msgs__rosidl_typesupport_fastrtps_cpp
#   my_msgs::my_msgs__rosidl_typesupport_fastrtps_c
# --------------------------------------------------------------------------
function(ibmw_add_rosidl_interface_target TARGET_NAME)
  cmake_parse_arguments(ARG "" "PACKAGE_NAME;GENERATOR_CPP_TARGET;FASTRTPS_CPP_TARGET;FASTRTPS_C_TARGET" "DEPS" ${ARGN})

  if(NOT ARG_PACKAGE_NAME)
    message(FATAL_ERROR "ibmw_add_rosidl_interface_target(${TARGET_NAME}): PACKAGE_NAME is required")
  endif()

  find_package(${ARG_PACKAGE_NAME} REQUIRED)

  if(NOT ARG_GENERATOR_CPP_TARGET)
    set(ARG_GENERATOR_CPP_TARGET "${ARG_PACKAGE_NAME}::${ARG_PACKAGE_NAME}__rosidl_generator_cpp")
  endif()
  if(NOT ARG_FASTRTPS_CPP_TARGET)
    set(ARG_FASTRTPS_CPP_TARGET "${ARG_PACKAGE_NAME}::${ARG_PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp")
  endif()
  if(NOT ARG_FASTRTPS_C_TARGET)
    set(ARG_FASTRTPS_C_TARGET "${ARG_PACKAGE_NAME}::${ARG_PACKAGE_NAME}__rosidl_typesupport_fastrtps_c")
  endif()

  foreach(_ibmw_rosidl_target IN ITEMS
      "${ARG_GENERATOR_CPP_TARGET}"
      "${ARG_FASTRTPS_CPP_TARGET}"
      "${ARG_FASTRTPS_C_TARGET}")
    if(NOT TARGET "${_ibmw_rosidl_target}")
      message(FATAL_ERROR
        "ibmw_add_rosidl_interface_target(${TARGET_NAME}): missing rosidl target ${_ibmw_rosidl_target}")
    endif()
  endforeach()

  add_library(${TARGET_NAME} INTERFACE)
  if(DEFINED ${ARG_PACKAGE_NAME}_DIR)
    get_filename_component(_ibmw_rosidl_pkg_prefix "${${ARG_PACKAGE_NAME}_DIR}/../../.." ABSOLUTE)
    target_include_directories(${TARGET_NAME}
      INTERFACE "${_ibmw_rosidl_pkg_prefix}/include")
  endif()
  target_link_libraries(${TARGET_NAME}
    INTERFACE
      ${ARG_GENERATOR_CPP_TARGET}
      ${ARG_FASTRTPS_CPP_TARGET}
      ${ARG_FASTRTPS_C_TARGET}
      ${ARG_DEPS})
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_pkg(<name>
#   SRCS <source files...>
#   MODULES <module targets...>
#   [DEPS <link dependencies...>]
#   [OUTPUT_NAME <output name>]
# )
#
# Creates a SHARED library (.so) for pkg-mode, linked to ibmw::api::pkg
# and the specified module targets.
#
# Example:
#   ibmw_add_pkg(pubsub_pkg
#     SRCS src/pubsub_pkg/pkg_main.cc
#     MODULES publisher_module subscriber_module)
# --------------------------------------------------------------------------
function(ibmw_add_pkg TARGET_NAME)
  cmake_parse_arguments(ARG "" "OUTPUT_NAME" "SRCS;MODULES;DEPS" ${ARGN})

  if(NOT ARG_SRCS)
    message(FATAL_ERROR "ibmw_add_pkg(${TARGET_NAME}): SRCS is required")
  endif()

  add_library(${TARGET_NAME} SHARED ${ARG_SRCS})

  target_link_libraries(${TARGET_NAME}
    PRIVATE ibmw::api::pkg ${ARG_MODULES} ${ARG_DEPS})

  if(ARG_OUTPUT_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
  else()
    set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${TARGET_NAME})
  endif()
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_app(<name>
#   SRCS <source files...>
#   MODULES <module targets...>
#   [INCLUDE_DIR <include directory>]
# )
#
# Creates a standalone executable embedding the IBMW engine, linked to
# ibmw::core::engine and the specified module targets.
#
# Example:
#   ibmw_add_app(pubsub_app
#     SRCS src/pubsub_app/main.cc
#     MODULES publisher_module subscriber_module)
# --------------------------------------------------------------------------
function(ibmw_add_app TARGET_NAME)
  cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SRCS;MODULES" ${ARGN})

  if(NOT ARG_SRCS)
    message(FATAL_ERROR "ibmw_add_app(${TARGET_NAME}): SRCS is required")
  endif()

  add_executable(${TARGET_NAME} ${ARG_SRCS})

  if(ARG_INCLUDE_DIR)
    target_include_directories(${TARGET_NAME} PRIVATE ${ARG_INCLUDE_DIR})
  else()
    target_include_directories(${TARGET_NAME}
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  endif()

  target_link_libraries(${TARGET_NAME}
    PRIVATE ibmw::core::engine ${ARG_MODULES})
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_dds_messages(<name>
#   IDL_GENCODE_TARGET <target>
#   [CDR_IMPLS <CdrAux.ipp wrapper .cc files...>]
#   [TYPE_BRIDGES <type bridge header files...>]
#   [DEPS <additional dependencies...>]
# )
#
# Wraps the boilerplate for compiling DDS CDR serialization implementations.
#
# Two modes are supported (selected by presence/absence of CDR_IMPLS):
#
# 1) "Schema-shipped" mode (CDR_IMPLS omitted, recommended for IBMW-built-in
#    schemas like ibmw::schema::example_idl_gencode):
#    The IDL_GENCODE_TARGET is assumed to already carry the CDR symbols
#    (typically because its own CMakeLists declared `WRAP_IDLS ...` so the
#    auto-generated <Type>CdrImpl.cc TUs are baked into its STATIC archive).
#    This macro then creates an INTERFACE library that simply forwards link
#    deps — zero hand-written .cc, zero ODR risk.
#
# 2) "Bring-your-own-IDL" mode (CDR_IMPLS provided): a STATIC library is
#    created that compiles the wrapper TU(s) — required for IDL gencode
#    targets that did NOT opt into WRAP_IDLS (e.g. types using FastCDR v1
#    patterns the py post-processor cannot yet handle).
#
# For projects using pre-shipped IBMW schemas (ibmw::schema::*), prefer
# mode 1.  For projects generating their own IDL, combine with
# add_dds_idl_gencode_target() from DdsIdlGenCode.cmake and pass CDR_IMPLS.
#
# Example (mode 1, schema-shipped — preferred):
#   ibmw_add_dds_messages(example_event_cdr
#     IDL_GENCODE_TARGET ibmw::schema::example_idl_gencode)
#
# Example (mode 2, bring-your-own-IDL):
#   include(DdsIdlGenCode)
#   add_dds_idl_gencode_target(
#     TARGET_NAME my_msgs_idl
#     IDL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/idl/MyMsg.idl
#     GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode)
#
#   ibmw_add_dds_messages(my_msgs_cdr
#     IDL_GENCODE_TARGET my_msgs_idl
#     CDR_IMPLS src/my_msg_cdr_impl.cc)
# --------------------------------------------------------------------------
function(ibmw_add_dds_messages TARGET_NAME)
  cmake_parse_arguments(ARG "" "IDL_GENCODE_TARGET;INCLUDE_DIR" "CDR_IMPLS;TYPE_BRIDGES;DEPS" ${ARGN})

  if(NOT ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()

  # Core link deps shared by both modes.
  set(_deps ibmw::extensions::dds_types)
  if(ARG_IDL_GENCODE_TARGET)
    list(APPEND _deps ${ARG_IDL_GENCODE_TARGET})
  endif()
  if(ARG_DEPS)
    list(APPEND _deps ${ARG_DEPS})
  endif()

  if(NOT ARG_CDR_IMPLS)
    # Mode 1: INTERFACE forwarder. The IDL gencode target is expected to
    # already carry the CDR symbols (via WRAP_IDLS). Nothing to compile.
    add_library(${TARGET_NAME} INTERFACE)
    target_include_directories(${TARGET_NAME}
      INTERFACE $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)
    target_link_libraries(${TARGET_NAME} INTERFACE ${_deps})
    return()
  endif()

  # Mode 2: STATIC library with hand-written wrapper TUs (one .ipp #include each).
  add_library(${TARGET_NAME} STATIC ${ARG_CDR_IMPLS})

  target_include_directories(${TARGET_NAME}
    PUBLIC $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)

  set_target_properties(${TARGET_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

  target_link_libraries(${TARGET_NAME} PUBLIC ${_deps})
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_dds_rpc(<name>
#   RPC_GENCODE_TARGET <target>
#   [IDL_GENCODE_TARGET <target>]
#   [CDR_IMPLS <CdrAux.ipp wrapper .cc files...>]
#   [DEPS <additional dependencies...>]
# )
#
# Convenience wrapper for DDS RPC modules. Like ibmw_add_dds_messages() it
# supports two modes selected by the presence of CDR_IMPLS:
#   - omitted: INTERFACE forwarder, assumes RPC_GENCODE_TARGET (and the
#     underlying IDL_GENCODE_TARGET) already carry CDR symbols via WRAP_IDLS;
#   - provided: STATIC library that compiles the wrapper TU(s).
#
# Example (mode 1, schema-shipped — preferred):
#   ibmw_add_dds_rpc(example_rpc_cdr
#     IDL_GENCODE_TARGET ibmw::schema::example_idl_gencode
#     RPC_GENCODE_TARGET ibmw::schema::example_rpc_gencode)
#
# Example (mode 2, bring-your-own-IDL):
#   ibmw_add_dds_rpc(example_rpc_cdr
#     IDL_GENCODE_TARGET ibmw::schema::example_idl_gencode
#     RPC_GENCODE_TARGET ibmw::schema::example_rpc_gencode
#     CDR_IMPLS src/example_rpc_cdr_impl.cc)
# --------------------------------------------------------------------------
function(ibmw_add_dds_rpc TARGET_NAME)
  cmake_parse_arguments(ARG "" "IDL_GENCODE_TARGET;RPC_GENCODE_TARGET;INCLUDE_DIR" "CDR_IMPLS;DEPS" ${ARGN})

  if(NOT ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()

  set(_deps ibmw::extensions::dds_types)
  if(ARG_IDL_GENCODE_TARGET)
    list(APPEND _deps ${ARG_IDL_GENCODE_TARGET})
  endif()
  if(ARG_RPC_GENCODE_TARGET)
    list(APPEND _deps ${ARG_RPC_GENCODE_TARGET})
  endif()
  if(ARG_DEPS)
    list(APPEND _deps ${ARG_DEPS})
  endif()

  if(NOT ARG_CDR_IMPLS)
    add_library(${TARGET_NAME} INTERFACE)
    target_include_directories(${TARGET_NAME}
      INTERFACE $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)
    target_link_libraries(${TARGET_NAME} INTERFACE ${_deps})
    return()
  endif()

  add_library(${TARGET_NAME} STATIC ${ARG_CDR_IMPLS})

  target_include_directories(${TARGET_NAME}
    PUBLIC $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)

  set_target_properties(${TARGET_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

  target_link_libraries(${TARGET_NAME} PUBLIC ${_deps})
endfunction()

# --------------------------------------------------------------------------
# ibmw_add_json_types(<name>
#   [SRCS <source files...>]
#   [DEPS <additional dependencies...>]
# )
#
# Convenience for JSON type libraries.  JSON types are header-only
# (struct + NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT), so this simply
# creates an INTERFACE target that pulls in nlohmann_json and ibmw::api::cpp.
#
# If SRCS are provided (e.g. you have .cc files that compile type definitions),
# a STATIC library is created instead.
#
# Example (header-only, recommended):
#   ibmw_add_json_types(my_json_types)
#   # Then in modules: target_link_libraries(my_mod PUBLIC my_json_types)
#
# Typically JSON types are just headers included directly, so this macro
# is optional — it exists for symmetry with ibmw_add_dds_messages().
# --------------------------------------------------------------------------
function(ibmw_add_json_types TARGET_NAME)
  cmake_parse_arguments(ARG "" "INCLUDE_DIR" "SRCS;DEPS" ${ARGN})

  if(NOT ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()

  if(ARG_SRCS)
    add_library(${TARGET_NAME} STATIC ${ARG_SRCS})
    target_include_directories(${TARGET_NAME}
      PUBLIC $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)
    set_target_properties(${TARGET_NAME} PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${TARGET_NAME}
      PUBLIC ibmw::api::cpp nlohmann_json::nlohmann_json ${ARG_DEPS})
  else()
    add_library(${TARGET_NAME} INTERFACE)
    target_include_directories(${TARGET_NAME}
      INTERFACE $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)
    target_link_libraries(${TARGET_NAME}
      INTERFACE ibmw::api::cpp nlohmann_json::nlohmann_json ${ARG_DEPS})
  endif()
endfunction()
