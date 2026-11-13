# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Multi-transport end-to-end benchmark suite. Seven pairs of publisher/
# subscriber samples whose CSV outputs feed the
# docs/transport-benchmark.{md,zh.md} comparison matrix:
#
#   iox_loaned_pod  - iceoryx: zero-copy POD path (extension-only API)
#   iox_loaned_ctx  - iceoryx: zero-copy ctx-meta path (standard receive stack)
#   shm_loaned_pod  - shm_ext: iox_loaned_pod on the self-developed transport
#   iox_send        - iceoryx: standard Send() (with serialization)
#   dds_loaned      - DDS:     LoanedMessage path (zero-copy via DataSharing/SHM)
#   dds_send_shm    - DDS:     standard Send() over SHM transport (no DataSharing)
#   dds_send_tcp    - DDS:     standard Send() over TCP-only transport
#   net_send_tcp    - net_ext: standard Send() over native net_extension (raw TCP)
#
# Gated by IBMW_BUILD_BENCHMARKS (declared in top-level CMakeLists), except
# for the OHOS ROS2-types/DDS endpoint-B-only path. The samples are added in
# stages 6.2.2..6.2.7.

set(_IBMW_BUILD_OHOS_ABC_DDS_ENDPOINT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS" AND
   IBMW_BUILD_ROS2_TYPES AND IBMW_BUILD_DDS_EXTENSION)
  set(_IBMW_BUILD_OHOS_ABC_DDS_ENDPOINT ON)
endif()

if(NOT IBMW_BUILD_BENCHMARKS AND
   NOT _IBMW_BUILD_OHOS_ABC_DDS_ENDPOINT)
  message(STATUS "Skip transport_benchmark because IBMW_BUILD_BENCHMARKS is OFF")
  return()
endif()

set_namespace()

if(_IBMW_BUILD_OHOS_ABC_DDS_ENDPOINT)
  add_subdirectory(abc_interop)
  if(NOT TARGET ibmw_topology_dds_endpoint)
    add_subdirectory(topology_interop)
  endif()
  if(NOT IBMW_BUILD_BENCHMARKS)
    return()
  endif()
endif()

add_subdirectory(bench_payload)
add_subdirectory(bench_common)

# Reporter V1 hot-hook benchmark of the Core Metrics gates (iceoryx and shm); it
# links no transport. Build the compile-out baseline and the fully enabled
# Reporter lane. A Core Metrics build without Reporter V1 has no Reporter hot
# path to benchmark.
if((IBMW_BUILD_ICEORYX_EXTENSION OR IBMW_BUILD_SHM_EXTENSION) AND
   (NOT IBMW_ENABLE_CORE_METRICS OR IBMW_ENABLE_REPORTER_V1))
  add_executable(core_metrics_reporter_hot_bench
    iox_loaned_ctx/standalone/reporter_hot_bench/main.cc)
  target_link_libraries(core_metrics_reporter_hot_bench PRIVATE
    ibmw::core::engine ibmw::core::extension_api ibmw::core::reporter_host_abi)
  set_target_properties(core_metrics_reporter_hot_bench PROPERTIES ENABLE_EXPORTS ON)
  if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_options(core_metrics_reporter_hot_bench PRIVATE
      "LINKER:--export-dynamic")
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  add_subdirectory(iox_loaned_ctx)
  return()
endif()

# In-process microbenchmark for the dispatcher publish-tap hot path.  This is
# deliberately a benchmark executable rather than a CTest: results are
# machine-local evidence and are never compared with a repository-wide fixed
# threshold.  It reuses the same LatencyStats implementation as the transport
# matrix and exercises the real dispatcher RegisterSendType/Send path.
add_subdirectory(publish_tap)

# Sample sub-directories are added in stages 6.2.2 .. 6.2.7.
add_subdirectory(iox_loaned_pod)
add_subdirectory(iox_loaned_ctx)
add_subdirectory(iox_send)
# Self-developed shm transport (shm_ext) running the iox_send modules.
add_subdirectory(shm_send)
add_subdirectory(dds_loaned)
add_subdirectory(dds_send_shm)
add_subdirectory(dds_send_tcp)
# # add_subdirectory(net_send_tcp)  # 6.2.7 in progress  # 6.2.7 in progress
add_subdirectory(net_send_tcp)
# Phase 7 §7.3 Step 3.8: encapsulated user-level loan path benchmark.
add_subdirectory(iox_loan_v2)
add_subdirectory(shm_loan_v2)
add_subdirectory(shm_loaned_ctx)
add_subdirectory(shm_loaned_pod)

# Minimal cross-middleware A/B/C interop endpoint.  It is deliberately
# separate from the historical matrix so existing targets and ABI remain
# unchanged.
add_subdirectory(abc_interop)
if(IBMW_BUILD_ROS2_TYPES AND IBMW_BUILD_DDS_EXTENSION)
  add_subdirectory(topology_interop)
endif()

add_custom_target(
  ibmw_samples_cpp_transport_benchmark_strict_matrix_scripts ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/validate_transport_bench_matrix.sh
          ${CMAKE_BINARY_DIR}/validate_transport_bench_matrix.sh
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/dds_loaned/validate_dds_loaned_e2e.sh
          ${CMAKE_BINARY_DIR}/validate_dds_loaned_e2e.sh
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/dds_send_shm/validate_dds_send_shm_executor_e2e.sh
          ${CMAKE_BINARY_DIR}/validate_dds_send_shm_executor_e2e.sh
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/dds_send_tcp/validate_dds_send_tcp_executor_e2e.sh
          ${CMAKE_BINARY_DIR}/validate_dds_send_tcp_executor_e2e.sh)

foreach(_matrix_dep
        dds_loaned_publisher_app
        dds_loaned_subscriber_app
        dds_send_shm_publisher_app
        dds_send_shm_subscriber_app
        dds_send_tcp_publisher_app
        dds_send_tcp_subscriber_app
        iox_loaned_pod_publisher_app
        iox_loaned_pod_subscriber_app
        iox_loaned_ctx_publisher_app
        iox_loaned_ctx_subscriber_app
        iox_send_publisher_app
        iox_send_subscriber_app
        iox_loan_v2_publisher_app
        iox_loan_v2_subscriber_app
        shm_send_publisher_app
        shm_send_subscriber_app
        shm_loan_v2_publisher_app
        shm_loan_v2_subscriber_app
        shm_loaned_ctx_publisher_app
        shm_loaned_ctx_subscriber_app
        shm_loaned_pod_publisher_app
        shm_loaned_pod_subscriber_app
        net_send_tcp_publisher_app
        net_send_tcp_subscriber_app)
  if(TARGET ${_matrix_dep})
    add_dependencies(ibmw_samples_cpp_transport_benchmark_strict_matrix_scripts
                     ${_matrix_dep})
  endif()
endforeach()
