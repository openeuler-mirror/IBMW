# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iox_loaned_ctx: zero-copy ctx-meta benchmark for the iceoryx extension.
#
# Two-process layout (publisher + subscriber + iox-roudi):
#   - publisher_module constructs a typed CompressedImage and routes it
#     through IoxCtxMetaTypedPublisher, which borrows an iceoryx SHM
#     chunk, CDR-serializes the message DIRECTLY into it, emits the
#     ctx-meta header (pkg_size + sertype + KV pairs), and publishes --
#     end-to-end zero-copy on the publish side.
#   - subscriber_module uses the standard IBMW receive stack
#     (OnMessage<CompressedImage>); latency is measured via
#     MonotonicNs() - msg.header().time_stamp().
#
# Pairs with iox_loaned_pod (extension-only POD path) and iox_send
# (no zero-copy baseline) to isolate the contribution of the ctx-meta
# loan path in the benchmark matrix.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iox_loaned_ctx because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
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

add_executable(iox_loaned_ctx_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(iox_loaned_ctx_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loaned_ctx_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loaned_ctx_publisher_module)

add_executable(iox_loaned_ctx_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(iox_loaned_ctx_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loaned_ctx_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loaned_ctx_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iox_loaned_ctx only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iox_loaned_ctx_publisher_app
          iox_loaned_ctx_subscriber_app
          ibmw_extensions_iceoryx_ext)
