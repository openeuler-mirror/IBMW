# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# dds_send_shm: standard DDS Send (no LoanedMessage) baseline benchmark.
#
# Two-process layout (publisher + subscriber, no roudi -- DDS owns its
# own SHM segment via DataSharing):
#   - publisher_module constructs a typed BenchPlainPayload<N> and
#     routes it through the standard `ibmw::transport::Send()` path.
#     The dds_extension driver CDR-serializes into a heap BufferArray
#     (GenericDdsPubSubType<T>::serialize via FastCDR), then FastDDS
#     carries the bytes over its DDS transport (DataSharing if both
#     sides advertise SHM, UDP fallback otherwise). The publish path
#     is NOT zero-copy: there is at minimum one CDR encode + one
#     transport-side copy.
#   - subscriber_module mirrors dds_loaned exactly (standard
#     `OnMessage<PlainT>`), so any latency delta vs. dds_loaned is
#     attributable purely to the publish-side LoanedMessage vs.
#     CDR+memcpy path.
#
# Position in the matrix:
#   - dds_loaned -> DDS LoanedMessage / DataSharing zero-copy path
#   - dds_send_shm   -> DDS standard Send (this sample)
#   - tcp_send   -> DDS Send forced onto TCP transport (no SHM at all)
# Three iceoryx samples (iox_loaned_pod / iox_loaned_ctx / iox_send)
# round out the cross-transport context.

if(NOT IBMW_BUILD_DDS_EXTENSION)
  message(STATUS "Skip dds_send_shm because IBMW_BUILD_DDS_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
add_subdirectory(managed/publisher_module)
add_subdirectory(managed/subscriber_module)

# --- Standalone apps (separate processes) ---

add_executable(dds_send_shm_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(dds_send_shm_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(dds_send_shm_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_dds_extension
  ibmw_samples_cpp_transport_benchmark_dds_send_shm_publisher_module)

add_executable(dds_send_shm_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(dds_send_shm_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(dds_send_shm_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_dds_extension
  ibmw_samples_cpp_transport_benchmark_dds_send_shm_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "dds_send_shm only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS dds_send_shm_publisher_app
          dds_send_shm_subscriber_app
          ibmw_dds_extension)
