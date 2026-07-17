# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iox_loaned_pod: zero-copy POD benchmark for the iceoryx extension.
#
# Two-process layout (publisher + subscriber + iox-roudi):
#   - publisher_module borrows an SHM chunk via IoxRawPodPublisher,
#     writes a BenchPodHeader + filler bytes, and publishes -- no
#     IBMW serialization, no buffer-array allocator, no copy.
#   - subscriber_module registers a raw POD callback on the iceoryx
#     driver, computes latency = MonotonicNs() - send_ns, and
#     finalises one CSV row at Stop().

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iox_loaned_pod because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
  return()
endif()

# Get the current folder name (used to build per-target unique names and
# the build_all custom-target that copies bin/ into the build root).
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# --- Module static libraries ---
add_subdirectory(managed/publisher_module)
add_subdirectory(managed/subscriber_module)

# --- Standalone apps (separate processes) ---

add_executable(iox_loaned_pod_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(iox_loaned_pod_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loaned_pod_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loaned_pod_publisher_module)

add_executable(iox_loaned_pod_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(iox_loaned_pod_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loaned_pod_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loaned_pod_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iox_loaned_pod only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iox_loaned_pod_publisher_app
          iox_loaned_pod_subscriber_app
          ibmw_extensions_iceoryx_ext)
