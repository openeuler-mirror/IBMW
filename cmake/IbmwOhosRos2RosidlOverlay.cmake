# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

option(IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD
  "Enable the opt-in OHOS ROS2 geometry_msgs rosidl overlay prepare target."
  OFF)

set(IBMW_OHOS_ROS2_UNDERLAY_PREFIX "" CACHE PATH
  "Existing OHOS ROS2 underlay prefix used by the geometry_msgs overlay helper.")
set(IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE "" CACHE PATH
  "geometry_msgs source directory consumed by the OHOS ROS2 overlay helper.")
set(IBMW_OHOS_ROS2_PYTHONPATH "" CACHE PATH
  "Python module path for target ROS2 ament/rosidl packages used by the helper.")
set(IBMW_OHOS_ROS2_OVERLAY_PREFIX
  "${CMAKE_BINARY_DIR}/ros2_geometry_overlay"
  CACHE PATH
  "Helper-owned install prefix for the OHOS geometry_msgs rosidl overlay.")

set(IBMW_OHOS_SDK_NATIVE_ROOT "" CACHE PATH
  "OpenHarmony SDK native root for the OHOS ROS2 overlay helper. Falls back to OHOS_SDK_NATIVE_ROOT when unset.")
set(IBMW_OHOS_PYTHON_EXECUTABLE "" CACHE FILEPATH
  "Host Python interpreter used by the OHOS ROS2 overlay helper for rosidl code generation.")
set(IBMW_OHOS_PYTHON_INCLUDE_DIR "" CACHE PATH
  "Optional Python include directory forwarded for compatibility with OHOS ROS2 validation scripts.")
set(IBMW_OHOS_PYTHON_LIBRARY "" CACHE FILEPATH
  "Optional Python library forwarded for compatibility with OHOS ROS2 validation scripts.")
set(IBMW_OHOS_OPENSSL_ROOT_DIR "" CACHE PATH
  "Optional OpenSSL root forwarded to the OHOS ROS2 overlay helper.")
set(IBMW_OHOS_OPENSSL_INCLUDE_DIR "" CACHE PATH
  "Optional OpenSSL include directory forwarded to the OHOS ROS2 overlay helper.")
set(IBMW_OHOS_OPENSSL_SSL_LIBRARY "" CACHE FILEPATH
  "Optional OpenSSL SSL library forwarded to the OHOS ROS2 overlay helper.")
set(IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY "" CACHE FILEPATH
  "Optional OpenSSL crypto library forwarded to the OHOS ROS2 overlay helper.")

function(ibmw_ohos_ros2_rosidl_overlay_configure)
  if(NOT IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD)
    return()
  endif()

  if(NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
      "IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD=ON requires a cross OHOS configure. "
      "Native/default builds must leave this option OFF.")
  endif()

  if(NOT DEFINED OHOS_PLATFORM OR NOT OHOS_PLATFORM STREQUAL "OHOS")
    message(FATAL_ERROR
      "IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD=ON requires the OHOS toolchain context "
      "(OHOS_PLATFORM=OHOS).")
  endif()

  set(_ibmw_python_executable "${IBMW_OHOS_PYTHON_EXECUTABLE}")
  if(NOT _ibmw_python_executable AND DEFINED PYTHON_EXECUTABLE)
    set(_ibmw_python_executable "${PYTHON_EXECUTABLE}")
  endif()
  if(NOT _ibmw_python_executable AND DEFINED Python3_EXECUTABLE)
    set(_ibmw_python_executable "${Python3_EXECUTABLE}")
  endif()

  set(_ibmw_python_include_dir "${IBMW_OHOS_PYTHON_INCLUDE_DIR}")
  if(NOT _ibmw_python_include_dir AND DEFINED PYTHON_INCLUDE_DIR)
    set(_ibmw_python_include_dir "${PYTHON_INCLUDE_DIR}")
  endif()
  if(NOT _ibmw_python_include_dir AND DEFINED Python3_INCLUDE_DIR)
    set(_ibmw_python_include_dir "${Python3_INCLUDE_DIR}")
  endif()

  set(_ibmw_python_library "${IBMW_OHOS_PYTHON_LIBRARY}")
  if(NOT _ibmw_python_library AND DEFINED PYTHON_LIBRARY)
    set(_ibmw_python_library "${PYTHON_LIBRARY}")
  endif()
  if(NOT _ibmw_python_library AND DEFINED Python3_LIBRARY)
    set(_ibmw_python_library "${Python3_LIBRARY}")
  endif()

  set(_ibmw_openssl_root_dir "${IBMW_OHOS_OPENSSL_ROOT_DIR}")
  if(NOT _ibmw_openssl_root_dir AND DEFINED OPENSSL_ROOT_DIR)
    set(_ibmw_openssl_root_dir "${OPENSSL_ROOT_DIR}")
  endif()

  set(_ibmw_openssl_include_dir "${IBMW_OHOS_OPENSSL_INCLUDE_DIR}")
  if(NOT _ibmw_openssl_include_dir AND DEFINED OPENSSL_INCLUDE_DIR)
    set(_ibmw_openssl_include_dir "${OPENSSL_INCLUDE_DIR}")
  endif()

  set(_ibmw_openssl_ssl_library "${IBMW_OHOS_OPENSSL_SSL_LIBRARY}")
  if(NOT _ibmw_openssl_ssl_library AND DEFINED OPENSSL_SSL_LIBRARY)
    set(_ibmw_openssl_ssl_library "${OPENSSL_SSL_LIBRARY}")
  endif()

  set(_ibmw_openssl_crypto_library "${IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY}")
  if(NOT _ibmw_openssl_crypto_library AND DEFINED OPENSSL_CRYPTO_LIBRARY)
    set(_ibmw_openssl_crypto_library "${OPENSSL_CRYPTO_LIBRARY}")
  endif()

  foreach(_ibmw_required_var IN ITEMS
      IBMW_OHOS_ROS2_UNDERLAY_PREFIX
      IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE
      IBMW_OHOS_ROS2_PYTHONPATH
      IBMW_OHOS_ROS2_OVERLAY_PREFIX)
    if(NOT ${_ibmw_required_var})
      message(FATAL_ERROR
        "IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD=ON requires ${_ibmw_required_var}.")
    endif()
  endforeach()

  if(NOT _ibmw_python_executable)
    message(FATAL_ERROR
      "IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD=ON requires "
      "IBMW_OHOS_PYTHON_EXECUTABLE, PYTHON_EXECUTABLE, or Python3_EXECUTABLE.")
  endif()

  set(_ibmw_ohos_sdk_native_root "${IBMW_OHOS_SDK_NATIVE_ROOT}")
  if(NOT _ibmw_ohos_sdk_native_root AND DEFINED OHOS_SDK_NATIVE_ROOT)
    set(_ibmw_ohos_sdk_native_root "${OHOS_SDK_NATIVE_ROOT}")
  endif()
  if(NOT _ibmw_ohos_sdk_native_root)
    message(FATAL_ERROR
      "IBMW_ENABLE_OHOS_ROS2_ROSIDL_OVERLAY_BUILD=ON requires "
      "IBMW_OHOS_SDK_NATIVE_ROOT or OHOS_SDK_NATIVE_ROOT.")
  endif()

  set(_ibmw_overlay_helper
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh")
  if(NOT EXISTS "${_ibmw_overlay_helper}")
    message(FATAL_ERROR "OHOS ROS2 overlay helper not found: ${_ibmw_overlay_helper}")
  endif()

  if(NOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    set(IBMW_TARGET_ROS2_DEPS_PREFIX
      "${IBMW_OHOS_ROS2_OVERLAY_PREFIX};${IBMW_OHOS_ROS2_UNDERLAY_PREFIX}"
      CACHE STRING
      "Target-side prefix or CMake-list of prefixes containing ROS 2 type-support dependencies for cross builds."
      FORCE)
  endif()

  string(REPLACE ";" ":" _ibmw_ros2_prefix_path "${IBMW_TARGET_ROS2_DEPS_PREFIX}")
  set(ENV{AMENT_PREFIX_PATH} "${_ibmw_ros2_prefix_path}")
  set(ENV{PYTHONPATH} "${IBMW_OHOS_ROS2_PYTHONPATH}")

  add_custom_target(ibmw_ohos_ros2_geometry_overlay
    COMMAND ${CMAKE_COMMAND} -E env
      "AMENT_PREFIX_PATH=${_ibmw_ros2_prefix_path}"
      "PYTHONPATH=${IBMW_OHOS_ROS2_PYTHONPATH}"
      "IBMW_OHOS_ROS2_UNDERLAY_PREFIX=${IBMW_OHOS_ROS2_UNDERLAY_PREFIX}"
      "IBMW_OHOS_ROS2_OVERLAY_PREFIX=${IBMW_OHOS_ROS2_OVERLAY_PREFIX}"
      "IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE=${IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE}"
      "IBMW_OHOS_ROS2_PYTHONPATH=${IBMW_OHOS_ROS2_PYTHONPATH}"
      "IBMW_OHOS_SDK_NATIVE_ROOT=${_ibmw_ohos_sdk_native_root}"
      "IBMW_OHOS_PYTHON_EXECUTABLE=${_ibmw_python_executable}"
      "IBMW_OHOS_PYTHON_INCLUDE_DIR=${_ibmw_python_include_dir}"
      "IBMW_OHOS_PYTHON_LIBRARY=${_ibmw_python_library}"
      "IBMW_OHOS_OPENSSL_ROOT_DIR=${_ibmw_openssl_root_dir}"
      "IBMW_OHOS_OPENSSL_INCLUDE_DIR=${_ibmw_openssl_include_dir}"
      "IBMW_OHOS_OPENSSL_SSL_LIBRARY=${_ibmw_openssl_ssl_library}"
      "IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY=${_ibmw_openssl_crypto_library}"
      bash "${_ibmw_overlay_helper}" --prepare "${CMAKE_BINARY_DIR}"
    COMMENT "Preparing OHOS geometry_msgs ROS2 rosidl overlay"
    VERBATIM)

  set_property(TARGET ibmw_ohos_ros2_geometry_overlay PROPERTY FOLDER ohos_ros2)
endfunction()
