# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# dds_loaned: zero-copy DDS LoanedMessage benchmark.
#
# Two-process layout (publisher + subscriber, no roudi):
#   - publisher_module borrows a FastDDS LoanedMessage<PlainT>, writes
#     the bench fields directly into the SHM chunk via DataSharing
#     (FastDDS' built-in SHM transport), and publishes via std::move.
#     End-to-end zero-copy on the publish side, no engine CDR pipeline.
#   - subscriber_module uses the standard IBMW receive stack
#     (OnMessage<PlainT>); latency is measured via
#     MonotonicNs() - msg.send_ns().
#
# The 5 typed slots map to 5 distinct FastDDS DataWriters under the
# same IBMW topic name (driver suffixes "_native_<sanitized_type>" in
# native mode), giving us byte-identical apples-to-apples comparison
# with iox_loaned_ctx / iox_send / dds_send / tcp_send.
#
# Pairs with iox_loaned_pod (extension-only POD path) for cross-
# transport zero-copy comparison in the benchmark matrix.

if(NOT IBMW_BUILD_DDS_EXTENSION)
  message(STATUS "Skip dds_loaned because IBMW_BUILD_DDS_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
add_subdirectory(managed/publisher_module)
add_subdirectory(managed/subscriber_module)

# --- Standalone apps (separate processes) ---

add_executable(dds_loaned_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(dds_loaned_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)

target_link_libraries(dds_loaned_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_dds_extension
  ibmw_samples_cpp_transport_benchmark_dds_loaned_publisher_module)

add_executable(dds_loaned_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(dds_loaned_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(dds_loaned_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_dds_loaned_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "dds_loaned only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS dds_loaned_publisher_app
          dds_loaned_subscriber_app
          ibmw_dds_extension)
