# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

if(IBMW_BUILD_DDS_XML_VALIDATION)
  add_subdirectory(dds_xml_validation)
endif()

if(IBMW_BUILD_EXAMPLES)
  add_subdirectory(quickstart)
  add_subdirectory(executor_demo)
  add_subdirectory(logging_demo)
  add_subdirectory(config_param_demo)
  add_subdirectory(engine_smoke_test)
  add_subdirectory(executor_conformance_test)

  # DDS examples (requires DDS extension)
  if(IBMW_BUILD_DDS_EXTENSION)
    add_subdirectory(dds_native_demo)
    add_subdirectory(dds_image_demo)
    add_subdirectory(context_demo)
    add_subdirectory(transport_demo)
  endif()

  # Iceoryx examples (requires Iceoryx extension; reuses dds_types for IDL
  # type registration, hence DDS extension OR Iceoryx alone is sufficient as
  # long as dds_types interface library is present).
  if(IBMW_BUILD_ICEORYX_EXTENSION)
    add_subdirectory(iceoryx_inproc_demo)
    add_subdirectory(iceoryx_interproc_demo)
  endif()

  # Multi-transport end-to-end benchmark suite (seven pub/sub sample pairs
  # comparing iceoryx / DDS / net_extension paths, feeding
  # docs/transport-benchmark.{md,zh.md}). Gated by IBMW_BUILD_BENCHMARKS.
  if(IBMW_BUILD_BENCHMARKS)
    add_subdirectory(transport_benchmark)
    # r8_validation links the benchmark's bench_payload target, so load it
    # only after transport_benchmark has created that dependency.
    add_subdirectory(r8_validation)
  endif()

  # ROS2 type-only examples (rosidl build-time, no rcl runtime)
  if(IBMW_BUILD_ROS2_INTEROP_DEMO)
    add_subdirectory(ros2_interop_demo)
  endif()

  if(IBMW_BUILD_ROS2_POINT_DEMO)
    add_subdirectory(ros2_point_demo)
  endif()

  if(IBMW_BUILD_LOANED_MESSAGE_DEMO)
    add_subdirectory(loaned_message_demo)
  endif()

  # ROS2 rcl-dependent examples (need rclcpp runtime)
  if(IBMW_BUILD_WITH_ROS2)
    add_subdirectory(ros2_chn)
    add_subdirectory(ros2_rpc)
  endif()

  # Mixed-transport record/info demonstration. This sample requires the
  # complete DDS, Iceoryx, MQTT and ROS2 recording matrix.
  if(IBMW_BUILD_DDS_EXTENSION AND
     IBMW_BUILD_PROTOCOLS AND
     IBMW_BUILD_ICEORYX_EXTENSION AND
     IBMW_BUILD_MQTT_EXTENSION AND
     IBMW_BUILD_RECORD_PLAYBACK_EXTENSION AND
     IBMW_BUILD_WITH_ROS2 AND
     IBMW_BUILD_ROS2_EXTENSION AND
     IBMW_BUILD_ROS2_TYPES)
    add_subdirectory(record_demo)
  endif()

  # Pre-recorded MCAP/SQLite3 playback demonstration. It only needs DDS for
  # the external playback route and the record/playback extension for bag CLI.
  if(IBMW_BUILD_DDS_EXTENSION AND
     IBMW_BUILD_PROTOCOLS AND
     IBMW_BUILD_RECORD_PLAYBACK_EXTENSION)
    add_subdirectory(playback_demo)
  endif()
endif()

# Private ROS2 Action Fibonacci proof samples. The forward DDS-only server path
# needs DDS transport plus ROS2 build-time action type availability, but its
# native ROS2 client validation peer is sample-local gated by IBMW_BUILD_WITH_ROS2.
if(IBMW_BUILD_DDS_EXTENSION AND IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES)
  add_subdirectory(ros2_action_fibonacci_common)
  add_subdirectory(ros2_action_fibonacci_dds_server)
endif()

# The reverse DDS-only client path needs DDS transport plus ROS2 build-time
# action type availability; its native ROS2 server validation peer is
# sample-local gated by IBMW_BUILD_WITH_ROS2.
if(IBMW_BUILD_DDS_EXTENSION AND IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES)
  add_subdirectory(ros2_action_fibonacci_dds_client)
endif()

if(IBMW_BUILD_EXAMPLES AND IBMW_BUILD_DDS_EXTENSION AND IBMW_BUILD_ICEORYX_EXTENSION)
  add_subdirectory(multi_driver_demo)
endif()

if(IBMW_BUILD_RETAINED_LOAN_E2E)
  add_subdirectory(retained_loan_e2e)
endif()
