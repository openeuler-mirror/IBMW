# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# ibmw::internal — INTERFACE library that hosts headers used only by
# alias-forwarding compatibility shims. See
# docs/contributing/architecture-rules.md.
#
# Public-API contract:
#   - Files under src/internal/typesupport/<transport>/ install to
#     <prefix>/include/ibmw/internal/typesupport/<transport>/.
#   - Each file MUST carry an INTERNAL banner explicitly disclaiming
#     public-API status; the headers are only consumed transitively via
#     the alias headers that live under api/cpp/util/ and
#     extensions/transport/<t>_types/transport/.
#   - Sunset: remove this subdirectory and its install destination after all
#     in-tree consumers and ibmw-template migrate off alias paths.
# DO NOT add new public headers here. New public headers belong under
# src/api/ and follow the standard public-header relocation policy.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name (ibmw_internal / ibmw::internal)
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection for the installable compatibility surface only.
# Do not recurse across all of src/internal: newer internal helpers such as
# ros2_action_dds are build-only and must not be exported through ibmw::internal.
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/typesupport/*.h)
file(GLOB ros2_action_dds_head_files ${CMAKE_CURRENT_SOURCE_DIR}/ros2_action_dds/*.h)

# Add target
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set include path of target.
# BUILD_INTERFACE: src/  -> consumers can include "internal/typesupport/..."
# INSTALL_INTERFACE: include/ibmw -> same path resolves post-install
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)

# Set head files of target. BASE_DIRS = src/internal, so files install
# preserving the "typesupport/<t>/<t>_transport.h" suffix.
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# NOTE: ibmw::internal intentionally does NOT link to any other ibmw
# target. It is a pure include-path/headers provider. Consumers
# (ibmw::api::cpp, ibmw::extensions::transport::dds_types,
# ibmw::extensions::transport::ros2_types) already link the upstream
# dependencies (fastcdr/fastdds) that internal headers reference.
# Linking ibmw::api::cpp here would create a cycle because
# ibmw::api::cpp links ibmw::internal to forward its alias headers.

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
endif()

# Build-only internal ROS 2 Action DDS helper target. It intentionally has no
# INSTALL_INTERFACE include path, FILE_SET, export name, or install() rule.
add_library(${CUR_TARGET_NAME}_ros2_action_dds INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME}::ros2_action_dds ALIAS ${CUR_TARGET_NAME}_ros2_action_dds)

target_include_directories(
  ${CUR_TARGET_NAME}_ros2_action_dds
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

target_sources(
  ${CUR_TARGET_NAME}_ros2_action_dds
  INTERFACE ${ros2_action_dds_head_files})
