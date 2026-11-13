# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# IbmwTransportFlags — transport capability configuration.
#
# Defines the `ibmw::config` INTERFACE library plus a generated public
# header `<ibmw/transport_features.h>` that exposes compile-time
# capability macros for transport availability:
#
#   IBMW_HAS_DDS, IBMW_HAS_ROS2, IBMW_HAS_ICEORYX, IBMW_HAS_ZENOH,
#   IBMW_HAS_MQTT, IBMW_HAS_NET   (from IBMW_BUILD_*_EXTENSION options)
#   IBMW_HAS_LOCAL                (always 1; local transport has no flag)
#   IBMW_HAS_SHM                  (= IBMW_HAS_ICEORYX || IBMW_HAS_SELFDEV_SHM; SHM-DDS reserved)
#   IBMW_LOANED_SHM_OPT_OUT       (integer shim for IBMW_ALLOW_LOANED_WITHOUT_SHM
#                                  so it can be referenced inside C++ constant
#                                  expressions; the `defined` operator is
#                                  illegal inside `static_assert` per ISO C++
#                                  [cpp.cond]/1)
#
# The header is generated via configure_file() from
# cmake/transport_features.h.in and lives at
# ${CMAKE_BINARY_DIR}/include/ibmw/transport_features.h.
#
# Public-API contract:
#   - Consumers obtain the header by linking ibmw::config (or any target
#     that transitively links it, e.g. ibmw::api::cpp).
#   - The header is installed to <prefix>/include/ibmw/transport_features.h
#     so find_package(ibmw) consumers see it identically.
#   - The macros are integer-valued (`#cmakedefine01`) so they are safe
#     in any C/C++ constant expression: `#if IBMW_HAS_DDS`, ternary,
#     `static_assert`, template SFINAE.
#
# See docs/contributing/architecture-rules.md for design rationale.

# Map IBMW_BUILD_*_EXTENSION (on/off) -> IBMW_HAS_* (1/0) for configure_file.
# `set(... ${VAR})` carries the truthiness through to #cmakedefine01.
set(IBMW_HAS_DDS     ${IBMW_BUILD_DDS_EXTENSION})
set(IBMW_HAS_ROS2    ${IBMW_BUILD_ROS2_EXTENSION})
set(IBMW_HAS_ICEORYX ${IBMW_BUILD_ICEORYX_EXTENSION})
set(IBMW_HAS_SELFDEV_SHM ${IBMW_BUILD_SHM_EXTENSION})
set(IBMW_HAS_ZENOH   ${IBMW_BUILD_ZENOH_EXTENSION})
set(IBMW_HAS_MQTT    ${IBMW_BUILD_MQTT_EXTENSION})
set(IBMW_HAS_NET     ${IBMW_BUILD_NET_EXTENSION})

# Generate the public capability header.
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/transport_features.h.in
  ${CMAKE_BINARY_DIR}/include/ibmw/transport_features.h
  @ONLY)

# Define ibmw::config INTERFACE library.
add_library(ibmw_config INTERFACE)
add_library(ibmw::config ALIAS ibmw_config)

target_include_directories(
  ibmw_config
  INTERFACE $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>)

# Install: place the generated header next to other public headers and
# export the target so find_package(ibmw) downstream consumers see it.
if(IBMW_INSTALL)
  install(
    FILES ${CMAKE_BINARY_DIR}/include/ibmw/transport_features.h
    DESTINATION include/ibmw)

  set_property(TARGET ibmw_config PROPERTY EXPORT_NAME config)
  install(
    TARGETS ibmw_config
    EXPORT ${INSTALL_CONFIG_NAME})
endif()
