# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# GetFastDDS.cmake - Find FastDDS and FastCDR
#
# FastCDR is the core serialization library used by the IBMW type system.
# FastDDS is required for the DDS extension and optionally for the type system.
#
# Native/default builds use installed packages exactly as before. The optional
# IBMW source-prefix lane may provide target-side packages under
# IBMW_THIRD_PARTY_SOURCE_PREFIX; it is never enabled implicitly in native mode.

message(STATUS "get FastDDS/FastCDR ...")

# Route B cross builds must consume the target ROS 2 DDS provider first.  Use
# explicit imported library targets from IBMW_TARGET_ROS2_DEPS_PREFIX instead of
# package configs here: some ROS2 OHOS exports share eProsima_atomic across
# fastcdr/fastrtps and fail if another dependency has already defined it.
if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  ibmw_resolve_target_ros2_library(_ibmw_target_fastcdr fastcdr)
  ibmw_resolve_target_ros2_library(_ibmw_target_fastrtps fastrtps)
  ibmw_resolve_target_ros2_include_root(_ibmw_target_fastdds_include fastdds/dds/domain/DomainParticipant.hpp)

  if(NOT TARGET ibmw_target_fastcdr)
    add_library(ibmw_target_fastcdr SHARED IMPORTED GLOBAL)
    set_target_properties(ibmw_target_fastcdr PROPERTIES
      IMPORTED_LOCATION "${_ibmw_target_fastcdr}"
      INTERFACE_INCLUDE_DIRECTORIES "${_ibmw_target_fastdds_include}")
  endif()

  if(NOT TARGET ibmw_target_fastrtps)
    add_library(ibmw_target_fastrtps SHARED IMPORTED GLOBAL)
    set_target_properties(ibmw_target_fastrtps PROPERTIES
      IMPORTED_LOCATION "${_ibmw_target_fastrtps}"
      INTERFACE_INCLUDE_DIRECTORIES "${_ibmw_target_fastdds_include}"
      INTERFACE_LINK_LIBRARIES ibmw_target_fastcdr)
  endif()

  set(IBMW_FASTCDR_PROVIDER_TARGET ibmw_target_fastcdr)
  set(IBMW_FASTDDS_PROVIDER_TARGET ibmw_target_fastrtps)
  set(fastcdr_FOUND TRUE)
  set(fastrtps_FOUND TRUE)
  set(fastdds_FOUND TRUE)
  get_filename_component(_ibmw_target_fastcdr_real "${_ibmw_target_fastcdr}" REALPATH)
  get_filename_component(_ibmw_target_fastrtps_real "${_ibmw_target_fastrtps}" REALPATH)
  # If a provider has no versioned real filename, leave the version empty.
  # DdsIdlGenCode.cmake intentionally treats an unknown FastCDR version as the
  # conservative v1 compatibility lane used by ROS 2 Humble.
  if(_ibmw_target_fastcdr_real MATCHES "libfastcdr\\.so\\.([0-9.]+)$")
    set(fastcdr_VERSION "${CMAKE_MATCH_1}")
  endif()
  if(_ibmw_target_fastrtps_real MATCHES "libfastrtps\\.so\\.([0-9.]+)$")
    set(fastrtps_VERSION "${CMAKE_MATCH_1}")
    set(fastdds_VERSION "${CMAKE_MATCH_1}")
  endif()
  # Create alias for compatibility
  if(NOT TARGET fastcdr)
    add_library(fastcdr ALIAS ibmw_target_fastcdr)
  endif()
  if(NOT TARGET fastrtps)
    add_library(fastrtps ALIAS ibmw_target_fastrtps)
  endif()
  if(NOT TARGET fastdds)
    add_library(fastdds ALIAS ibmw_target_fastrtps)
  endif()
  message(STATUS "Found FastDDS via explicit target ROS2 fastrtps provider: ${_ibmw_target_fastrtps}")
endif()

if(NOT fastcdr_FOUND)
  ibmw_find_target_package(fastcdr QUIET FEATURE cdr-types
    HINTS /opt/ros/$ENV{ROS_DISTRO}/lib/cmake /opt/ros/humble/lib/cmake)
endif()
if(NOT fastcdr_FOUND)
  message(WARNING "FastCDR not found. CDR serialization (the primary type system) "
                  "will not be available. Install FastCDR: "
                  "apt install ros-humble-fastcdr or build from source.")
endif()

# FastDDS: try fastdds 3.x first, fallback to fastrtps for ROS2 compatibility
# Note: When ROS2 is sourced, fastrtps 2.6 defines eProsima_atomic target.
# Loading fastdds 3.x after that causes target conflict, so skip if already loaded.
if(NOT fastdds_FOUND AND NOT TARGET eProsima_atomic)
  ibmw_find_target_package(fastdds VERSION 3 QUIET FEATURE dds-extension)
endif()

if(NOT fastdds_FOUND)
  ibmw_find_target_package(fastrtps QUIET FEATURE dds-extension
    HINTS /opt/ros/$ENV{ROS_DISTRO}/share /opt/ros/humble/share)
  if(fastrtps_FOUND)
    set(fastdds_FOUND TRUE)
    # Create alias for compatibility
    if(NOT TARGET fastdds)
      add_library(fastdds ALIAS fastrtps)
    endif()
    message(STATUS "Found FastDDS via fastrtps: ${fastrtps_VERSION}")
  else()
    message(WARNING "FastDDS not found. DDS extension will not be available. "
                    "CDR serialization in the type system requires at least FastCDR.")
  endif()
endif()

if(fastdds_FOUND AND fastdds_VERSION)
  message(STATUS "Found FastDDS: ${fastdds_VERSION}")
endif()

if(fastcdr_FOUND)
  message(STATUS "Found FastCDR: ${fastcdr_VERSION}")
endif()

if(NOT IBMW_FASTCDR_PROVIDER_TARGET)
  set(IBMW_FASTCDR_PROVIDER_TARGET fastcdr)
endif()

if(NOT IBMW_FASTDDS_PROVIDER_TARGET)
  set(IBMW_FASTDDS_PROVIDER_TARGET fastdds)
endif()

# import targets:
# fastcdr        (always available - core CDR serialization, v1 from ROS Humble)
# fastdds        (optional - for DDS extension)
