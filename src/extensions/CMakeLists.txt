# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

# ── Transport extensions ─────────────────────────────────────────────

if(IBMW_BUILD_ROS2_TYPES)
  add_subdirectory(transport/ros2_types)
endif()

if(IBMW_BUILD_NET_EXTENSION OR IBMW_BUILD_GRPC_EXTENSION)
  add_subdirectory(transport/net_base)
endif()

if(IBMW_BUILD_NET_EXTENSION)
  add_subdirectory(transport/net_ext)
endif()

if(IBMW_BUILD_ROS2_EXTENSION)
  add_subdirectory(transport/ros2_ext)
endif()

if(IBMW_BUILD_MQTT_EXTENSION)
  add_subdirectory(transport/mqtt_ext)
endif()

if(IBMW_BUILD_ZENOH_EXTENSION)
  add_subdirectory(transport/zenoh_ext)
endif()

if(IBMW_BUILD_ICEORYX_EXTENSION)
  add_subdirectory(transport/iceoryx_ext)
endif()

# dds_types provides IDL-based type registration (IBMW_DDS_TYPE_NAME) and
# FastCDR serialization helpers used by both the DDS extension and the
# Iceoryx extension (Iceoryx pubsub demos register IDL-generated types
# through this header-only interface library).
if(IBMW_BUILD_DDS_EXTENSION OR IBMW_BUILD_ICEORYX_EXTENSION)
  add_subdirectory(transport/dds_types)
endif()

if(IBMW_BUILD_DDS_EXTENSION)
  add_subdirectory(transport/dds_ext)
endif()

# ── RPC extensions ───────────────────────────────────────────────────

if(IBMW_BUILD_GRPC_EXTENSION)
  add_subdirectory(rpc/grpc_ext)
endif()

# ── Observability extensions ─────────────────────────────────────────

if(IBMW_BUILD_LOG_CONTROL_EXTENSION)
  add_subdirectory(observability/log_control_ext)
endif()

if(IBMW_BUILD_TOPIC_LOGGER_EXTENSION)
  add_subdirectory(observability/topic_logger_ext)
endif()

if(IBMW_BUILD_OPENTELEMETRY_EXTENSION)
  add_subdirectory(observability/opentelemetry_ext)
endif()

# ── Management extensions ────────────────────────────────────────────

if(IBMW_BUILD_RECORD_PLAYBACK_EXTENSION)
  add_subdirectory(management/record_playback_ext)
endif()

if(IBMW_BUILD_TIME_MANIPULATOR_EXTENSION)
  add_subdirectory(management/time_manipulator_ext)
endif()

if(IBMW_BUILD_PARAMETER_EXTENSION)
  add_subdirectory(management/parameter_ext)
endif()

if(IBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION)
  add_subdirectory(management/service_introspection_ext)
endif()

# ── Utility extensions ───────────────────────────────────────────────

if(IBMW_BUILD_ECHO_EXTENSION)
  add_subdirectory(utility/echo_ext)
endif()

if(IBMW_BUILD_PROXY_EXTENSION)
  add_subdirectory(utility/proxy_ext)
endif()
