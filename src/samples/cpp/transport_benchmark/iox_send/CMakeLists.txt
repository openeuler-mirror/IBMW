# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iox_send: SHM-but-no-loaned baseline benchmark for the iceoryx extension.
#
# Two-process layout (publisher + subscriber + iox-roudi):
#   - publisher_module constructs a typed CompressedImage and routes it
#     through the standard `ibmw::transport::Send()` path, which
#     CDR-serializes into a heap buffer first and then memcpy's into an
#     iceoryx SHM chunk inside the driver. Same iceoryx transport, same
#     wire format, but the publish path is NOT zero-copy.
#   - subscriber_module is identical to iox_loaned_ctx (standard
#     `OnMessage<CompressedImage>`) so any latency delta vs.
#     iox_loaned_ctx isolates the publish-side loan vs. heap+memcpy
#     contribution.
#
# This is the third leg of the iceoryx benchmark triangle:
#   - iox_loaned_pod  -> driver-only POD path (no CDR)
#   - iox_loaned_ctx  -> CDR-into-loan zero-copy path
#   - iox_send        -> CDR-then-memcpy non-loan path  (this sample)
# The matrix is rounded out by dds_loaned / dds_send / tcp_send for
# cross-transport context.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iox_send because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
add_subdirectory(managed/publisher_module)
add_subdirectory(managed/subscriber_module)

# --- Standalone apps (separate processes) ---

add_executable(iox_send_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(iox_send_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_send_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_send_publisher_module)

add_executable(iox_send_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(iox_send_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_send_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_send_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iox_send only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iox_send_publisher_app
          iox_send_subscriber_app
          ibmw_extensions_iceoryx_ext)
