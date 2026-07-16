# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_TARGET_ROS2_ROOT "" CACHE PATH "Target ROS 2 installation root used for cross builds.")
set(IBMW_BUILD_TARGET_ROS2_DEPS OFF CACHE BOOL "Build target-side ROS 2 dependency scaffolding during cross builds.")
set(IBMW_TARGET_ROS2_DEPS_PREFIX "" CACHE STRING "Target-side prefix or CMake-list of prefixes containing ROS 2 type-support dependencies for cross builds.")
set(IBMW_HOST_ROS2_ROOT "" CACHE PATH "Host ROS 2 installation root used for build-time generators during cross builds.")
set(IBMW_TARGET_ROS2_TIER "none" CACHE STRING "Cross-build direct-feature tier: none, types, rosidl, or rclcpp.")
set_property(CACHE IBMW_TARGET_ROS2_TIER PROPERTY STRINGS none types rosidl rclcpp)

function(ibmw_cross_enable_target_ros2_prefixes)
  if(NOT CMAKE_CROSSCOMPILING)
    return()
  endif()

  set(_ibmw_target_ros2_prefixes)
  foreach(_ibmw_root IN LISTS IBMW_TARGET_ROS2_ROOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    if(_ibmw_root)
      list(APPEND _ibmw_target_ros2_prefixes "${_ibmw_root}")
    endif()
  endforeach()

  if(NOT _ibmw_target_ros2_prefixes)
    return()
  endif()

  list(REMOVE_DUPLICATES _ibmw_target_ros2_prefixes)
  list(PREPEND CMAKE_PREFIX_PATH ${_ibmw_target_ros2_prefixes})
  list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)

  list(PREPEND CMAKE_FIND_ROOT_PATH ${_ibmw_target_ros2_prefixes})
  list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
  set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}" PARENT_SCOPE)
endfunction()

function(ibmw_resolve_target_ros2_library OUT_VAR LIBRARY_NAME)
  set(_ibmw_target_ros2_library_roots)
  foreach(_ibmw_root IN LISTS IBMW_TARGET_ROS2_ROOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    if(_ibmw_root)
      list(APPEND _ibmw_target_ros2_library_roots "${_ibmw_root}")
    endif()
  endforeach()

  if(NOT _ibmw_target_ros2_library_roots)
    set(${OUT_VAR} "${LIBRARY_NAME}" PARENT_SCOPE)
    return()
  endif()

  list(REMOVE_DUPLICATES _ibmw_target_ros2_library_roots)
  set(_ibmw_target_ros2_library_paths)
  foreach(_ibmw_root IN LISTS _ibmw_target_ros2_library_roots)
    list(APPEND _ibmw_target_ros2_library_paths
      "${_ibmw_root}/lib"
      "${_ibmw_root}/lib64")
  endforeach()

  find_library(_ibmw_target_ros2_library_${LIBRARY_NAME}
    NAMES ${LIBRARY_NAME}
    PATHS ${_ibmw_target_ros2_library_paths}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)

  if(NOT _ibmw_target_ros2_library_${LIBRARY_NAME})
    message(FATAL_ERROR
      "Target ROS 2 library '${LIBRARY_NAME}' was not found under prefixes: "
      "${_ibmw_target_ros2_library_roots}")
  endif()

  set(${OUT_VAR} "${_ibmw_target_ros2_library_${LIBRARY_NAME}}" PARENT_SCOPE)
endfunction()

function(ibmw_resolve_target_ros2_include_root OUT_VAR HEADER_PATH)
  set(_ibmw_target_ros2_include_roots)
  foreach(_ibmw_root IN LISTS IBMW_TARGET_ROS2_ROOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    if(_ibmw_root)
      list(APPEND _ibmw_target_ros2_include_roots "${_ibmw_root}")
    endif()
  endforeach()

  if(NOT _ibmw_target_ros2_include_roots)
    set(${OUT_VAR} "" PARENT_SCOPE)
    return()
  endif()

  list(REMOVE_DUPLICATES _ibmw_target_ros2_include_roots)
  set(_ibmw_target_ros2_include_paths)
  foreach(_ibmw_root IN LISTS _ibmw_target_ros2_include_roots)
    list(APPEND _ibmw_target_ros2_include_paths "${_ibmw_root}/include")
  endforeach()

  find_path(_ibmw_target_ros2_include_root
    NAMES ${HEADER_PATH}
    PATHS ${_ibmw_target_ros2_include_paths}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
    NO_CACHE)

  if(NOT _ibmw_target_ros2_include_root)
    message(FATAL_ERROR
      "Target ROS 2 include root for '${HEADER_PATH}' was not found under prefixes: "
      "${_ibmw_target_ros2_include_roots}")
  endif()

  set(${OUT_VAR} "${_ibmw_target_ros2_include_root}" PARENT_SCOPE)
endfunction()

function(ibmw_cross_sanitize_sysroot_imported_includes)
  if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSROOT)
    return()
  endif()

  set(_ibmw_sysroot_include "${CMAKE_SYSROOT}/usr/include")
  foreach(_ibmw_target IN LISTS ARGN)
    if(NOT TARGET ${_ibmw_target})
      continue()
    endif()

    get_target_property(_ibmw_imported_includes ${_ibmw_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _ibmw_imported_includes)
      continue()
    endif()

    set(_ibmw_sanitized_includes ${_ibmw_imported_includes})
    list(REMOVE_ITEM _ibmw_sanitized_includes "${_ibmw_sysroot_include}")
    if(NOT "${_ibmw_sanitized_includes}" STREQUAL "${_ibmw_imported_includes}")
      set_target_properties(${_ibmw_target} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_ibmw_sanitized_includes}")
    endif()
  endforeach()
endfunction()

function(ibmw_cross_replace_bare_link_library TARGET_NAME BARE_LIBRARY RESOLVED_LIBRARY)
  if(NOT CMAKE_CROSSCOMPILING OR NOT TARGET ${TARGET_NAME})
    return()
  endif()

  get_target_property(_ibmw_aliased_target ${TARGET_NAME} ALIASED_TARGET)
  if(_ibmw_aliased_target)
    set(TARGET_NAME ${_ibmw_aliased_target})
  endif()

  foreach(_ibmw_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_ibmw_link_libraries ${TARGET_NAME} ${_ibmw_link_property})
    if(NOT _ibmw_link_libraries)
      continue()
    endif()

    set(_ibmw_sanitized_libraries)
    set(_ibmw_changed FALSE)
    foreach(_ibmw_link_library IN LISTS _ibmw_link_libraries)
      if(_ibmw_link_library STREQUAL "${BARE_LIBRARY}")
        list(APPEND _ibmw_sanitized_libraries "${RESOLVED_LIBRARY}")
        set(_ibmw_changed TRUE)
      elseif(_ibmw_link_library STREQUAL "$<LINK_ONLY:${BARE_LIBRARY}>")
        list(APPEND _ibmw_sanitized_libraries "$<LINK_ONLY:${RESOLVED_LIBRARY}>")
        set(_ibmw_changed TRUE)
      else()
        list(APPEND _ibmw_sanitized_libraries "${_ibmw_link_library}")
      endif()
    endforeach()

    if(_ibmw_changed)
      set_target_properties(${TARGET_NAME} PROPERTIES
        ${_ibmw_link_property} "${_ibmw_sanitized_libraries}")
    endif()
  endforeach()
endfunction()

function(ibmw_cross_pin_rosidl_fastcdr_target TARGET_NAME FASTCDR_LIBRARY)
  if(NOT CMAKE_CROSSCOMPILING OR NOT TARGET ${TARGET_NAME})
    return()
  endif()

  get_target_property(_ibmw_aliased_target ${TARGET_NAME} ALIASED_TARGET)
  if(_ibmw_aliased_target)
    set(TARGET_NAME ${_ibmw_aliased_target})
  endif()

  get_property(_ibmw_pinned_targets GLOBAL PROPERTY IBMW_CROSS_ROSIDL_FASTCDR_PINNED_TARGETS)
  if("${TARGET_NAME}" IN_LIST _ibmw_pinned_targets)
    return()
  endif()
  list(APPEND _ibmw_pinned_targets ${TARGET_NAME})
  set_property(GLOBAL PROPERTY IBMW_CROSS_ROSIDL_FASTCDR_PINNED_TARGETS "${_ibmw_pinned_targets}")

  ibmw_cross_replace_bare_link_library(
    ${TARGET_NAME}
    fastcdr
    "${FASTCDR_LIBRARY}")

  foreach(_ibmw_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_ibmw_link_libraries ${TARGET_NAME} ${_ibmw_link_property})
    if(NOT _ibmw_link_libraries)
      continue()
    endif()

    foreach(_ibmw_link_library IN LISTS _ibmw_link_libraries)
      set(_ibmw_link_target "${_ibmw_link_library}")
      if(_ibmw_link_library MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
        set(_ibmw_link_target "${CMAKE_MATCH_1}")
      endif()
      if(TARGET ${_ibmw_link_target})
        ibmw_cross_pin_rosidl_fastcdr_target(${_ibmw_link_target} "${FASTCDR_LIBRARY}")
      endif()
    endforeach()
  endforeach()
endfunction()

function(ibmw_cross_pin_rosidl_fastcdr FASTCDR_LIBRARY)
  if(NOT CMAKE_CROSSCOMPILING
     OR NOT IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
    return()
  endif()

  foreach(_ibmw_target IN LISTS ARGN)
    ibmw_cross_pin_rosidl_fastcdr_target(${_ibmw_target} "${FASTCDR_LIBRARY}")
  endforeach()
endfunction()

macro(ibmw_find_target_package PACKAGE)
  set(_ibmw_options REQUIRED QUIET)
  set(_ibmw_one_value_args VERSION FEATURE)
  cmake_parse_arguments(_IBMW_FIND "${_ibmw_options}" "${_ibmw_one_value_args}" "" ${ARGN})

  set(_ibmw_find_flags)
  if(_IBMW_FIND_REQUIRED)
    list(APPEND _ibmw_find_flags REQUIRED)
  endif()
  if(_IBMW_FIND_QUIET)
    list(APPEND _ibmw_find_flags QUIET)
  endif()

  if(NOT _IBMW_FIND_FEATURE)
    set(_IBMW_FIND_FEATURE "unspecified")
  endif()

  if(NOT CMAKE_CROSSCOMPILING)
    if(_IBMW_FIND_VERSION)
      find_package(${PACKAGE} ${_IBMW_FIND_VERSION} ${_IBMW_FIND_UNPARSED_ARGUMENTS} ${_ibmw_find_flags})
    else()
      find_package(${PACKAGE} ${_IBMW_FIND_UNPARSED_ARGUMENTS} ${_ibmw_find_flags})
    endif()
  else()
    set(_ibmw_target_package_roots)

    foreach(_ibmw_root IN LISTS
        IBMW_TARGET_ROS2_ROOT
        IBMW_TARGET_ROS2_DEPS_PREFIX
        CMAKE_SYSROOT)
      if(_ibmw_root)
        list(APPEND _ibmw_target_package_roots "${_ibmw_root}")
      endif()
    endforeach()

    foreach(_ibmw_root IN LISTS CMAKE_FIND_ROOT_PATH)
      if(_ibmw_root)
        list(APPEND _ibmw_target_package_roots "${_ibmw_root}")
      endif()
    endforeach()

    if(_ibmw_target_package_roots)
      list(REMOVE_DUPLICATES _ibmw_target_package_roots)
    endif()

    if(NOT _ibmw_target_package_roots AND _IBMW_FIND_REQUIRED)
      message(FATAL_ERROR
        "Cross target package '${PACKAGE}' for feature '${_IBMW_FIND_FEATURE}' requires "
        "IBMW_TARGET_ROS2_ROOT, IBMW_TARGET_ROS2_DEPS_PREFIX, CMAKE_SYSROOT, "
        "or CMAKE_FIND_ROOT_PATH. Host package discovery is disabled in cross mode.")
    elseif(NOT _ibmw_target_package_roots)
      set(${PACKAGE}_FOUND FALSE)
      unset(${PACKAGE}_DIR CACHE)
      unset(${PACKAGE}_DIR)
      if(NOT _IBMW_FIND_QUIET)
        message(STATUS
          "Optional cross target package '${PACKAGE}' for feature '${_IBMW_FIND_FEATURE}' "
          "was not searched because no target package prefix is configured.")
      endif()
    endif()

    if(_ibmw_target_package_roots AND _IBMW_FIND_VERSION)
      find_package(${PACKAGE} ${_IBMW_FIND_VERSION} QUIET
        PATHS ${_ibmw_target_package_roots}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    elseif(_ibmw_target_package_roots)
      find_package(${PACKAGE} QUIET
        PATHS ${_ibmw_target_package_roots}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    endif()

    set(_ibmw_found_var "${PACKAGE}_FOUND")
    if(${_ibmw_found_var})
      foreach(_ibmw_pkg_path_var IN ITEMS
          ${PACKAGE}_DIR
          ${PACKAGE}_CONFIG
          ${PACKAGE}_CONSIDERED_CONFIGS)
        if(DEFINED ${_ibmw_pkg_path_var})
          foreach(_ibmw_pkg_path IN LISTS ${_ibmw_pkg_path_var})
            if(_ibmw_pkg_path MATCHES "^/opt/ros/humble(/|$)")
              message(FATAL_ERROR
                "Cross target package '${PACKAGE}' for feature '${_IBMW_FIND_FEATURE}' "
                "resolved to host ROS 2 Humble path '${_ibmw_pkg_path}'. Set "
                "IBMW_TARGET_ROS2_ROOT or IBMW_TARGET_ROS2_DEPS_PREFIX to a target prefix.")
            endif()
          endforeach()
        endif()
      endforeach()
    elseif(_IBMW_FIND_REQUIRED)
      message(FATAL_ERROR
        "Required cross target package '${PACKAGE}' for feature '${_IBMW_FIND_FEATURE}' "
        "was not found under target prefixes: ${_ibmw_target_package_roots}")
    endif()
  endif()
endmacro()

if(NOT CMAKE_CROSSCOMPILING)
  return()
endif()

if(NOT IBMW_TARGET_ROS2_TIER STREQUAL "none"
   AND NOT IBMW_TARGET_ROS2_TIER STREQUAL "types"
   AND NOT IBMW_TARGET_ROS2_TIER STREQUAL "rosidl"
   AND NOT IBMW_TARGET_ROS2_TIER STREQUAL "rclcpp")
  message(FATAL_ERROR
    "Invalid IBMW_TARGET_ROS2_TIER='${IBMW_TARGET_ROS2_TIER}'. "
    "Supported cross-build direct-feature tiers in this slice are: none, types, rosidl. "
    "The rclcpp tier is deferred.")
endif()

if(IBMW_TARGET_ROS2_TIER STREQUAL "rclcpp")
  message(FATAL_ERROR
    "IBMW_TARGET_ROS2_TIER=rclcpp is not supported in this cross-build slice. "
    "Full rclcpp cross support is deferred.")
endif()

if(IBMW_BUILD_WITH_ROS2)
  message(FATAL_ERROR
    "IBMW_BUILD_WITH_ROS2=ON is not supported while cross compiling in this slice. "
    "The direct rclcpp feature tier is deferred; use IBMW_BUILD_ROS2_TYPES=ON "
    "with IBMW_TARGET_ROS2_TIER=types or rosidl for type-generation scaffolding only.")
endif()

if(IBMW_BUILD_ROS2_TYPES)
  if(NOT IBMW_TARGET_ROS2_TIER STREQUAL "types"
     AND NOT IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
    message(FATAL_ERROR
      "IBMW_BUILD_ROS2_TYPES=ON under cross compiling requires "
      "IBMW_TARGET_ROS2_TIER=types or rosidl.")
  endif()

  if(NOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    message(FATAL_ERROR
      "IBMW_BUILD_ROS2_TYPES=ON under cross compiling requires an explicit "
      "IBMW_TARGET_ROS2_DEPS_PREFIX pointing at the target ROS 2 type-support prefix "
      "or CMake-list of prefixes.")
  endif()
endif()
