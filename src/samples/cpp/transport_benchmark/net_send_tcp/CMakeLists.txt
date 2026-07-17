# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# net_send_tcp: standard `Send<T>` over the IBMW native net_extension
# raw asio TCP transport driver. The only kernel-network row in the
# 7-sample matrix (every other row stays in user space via SHM or
# DataSharing).
#
# Two-process layout (publisher + subscriber, no roudi, no FastDDS):
#   - publisher_module constructs a typed BenchSendPayload<N> and
#     routes it through the standard `ibmw::transport::Send()` path.
#     The net_extension tcp driver CDR-serializes via the engine's
#     MessageTypeSupport "cdr" registration (FastCDR) into a heap
#     BufferArray, wraps it in a custom binary frame
#     (pattern + serialization_type + ctx-meta + payload bytes), and
#     writes it to a TCP client socket connected to the subscriber's
#     listen_port. NOT zero-copy: at minimum one CDR encode + one
#     kernel TCP send-buffer copy per message.
#   - subscriber_module mirrors dds_send_shm / dds_send_tcp exactly
#     (standard `OnMessage<T>`), so any latency delta vs. those rows
#     is attributable purely to the transport driver (net asio TCP
#     vs FastDDS SHM vs FastDDS TCP RTPS).
#
# Position in the 7-sample matrix:
#   - iox_loaned_pod   -> iceoryx LoanPodSample (POD zero-copy)
#   - iox_loaned_ctx   -> iceoryx LoanSerializedSampleWithCtx
#   - iox_send         -> iceoryx Send<T> (driver-side memcpy)
#   - dds_loaned       -> FastDDS LoanedMessage / DataSharing zero-copy
#   - dds_send_shm     -> FastDDS Send forced onto SharedMemoryTransport
#   - dds_send_tcp     -> FastDDS Send forced onto TCPv4
#   - net_send_tcp     -> native net_extension Send over raw asio TCP (this sample)

if(NOT IBMW_BUILD_NET_EXTENSION)
  message(STATUS "Skip net_send_tcp because IBMW_BUILD_NET_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
add_subdirectory(managed/publisher_module)
add_subdirectory(managed/subscriber_module)

# --- Standalone apps (separate processes) ---

add_executable(net_send_tcp_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(net_send_tcp_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(net_send_tcp_publisher_app PRIVATE
  ibmw::core::engine
  ibmw::extensions::net_ext
  ibmw_samples_cpp_transport_benchmark_net_send_tcp_publisher_module)

add_executable(net_send_tcp_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(net_send_tcp_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(net_send_tcp_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw::extensions::net_ext
  ibmw_samples_cpp_transport_benchmark_net_send_tcp_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "net_send_tcp only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS net_send_tcp_publisher_app
          net_send_tcp_subscriber_app
          ibmw::extensions::net_ext)
