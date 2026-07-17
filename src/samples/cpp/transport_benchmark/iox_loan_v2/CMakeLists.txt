# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# iox_loan_v2: IBMW user-level zero-copy loan benchmark (Phase 7 §7.3).
#
# Two-process layout (publisher + subscriber + iox-roudi):
#   - publisher_module borrows an SHM chunk via the public
#     ibmw::transport::LoanedSample<T> + SenderRef::Loan/PublishLoaned
#     wrapper -- the same loan path any user-application would take.
#     Internally this dispatches to the iceoryx driver's LoanSample
#     (kPodOnly), so the wire URL and payload layout match
#     iox_loaned_pod byte-for-byte; the difference is purely the
#     publish-side API surface (encapsulated vs. extension-only).
#   - subscriber_module reuses iox_loaned_pod's RegisterRawPodSubscriber
#     callback (Step 3 ships only the publish-side wrapper). Latency
#     math is identical so the two samples are run-for-run comparable.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip iox_loan_v2 because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
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

add_executable(iox_loan_v2_publisher_app
  standalone/publisher_app/main.cc)

target_include_directories(iox_loan_v2_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loan_v2_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loan_v2_publisher_module)

add_executable(iox_loan_v2_subscriber_app
  standalone/subscriber_app/main.cc)

target_include_directories(iox_loan_v2_subscriber_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed)

target_link_libraries(iox_loan_v2_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_iox_loan_v2_subscriber_module)

# --- Install resources (cfg/, start_*.sh) into the build root ---

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "iox_loan_v2 only supports Linux")
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS iox_loan_v2_publisher_app
          iox_loan_v2_subscriber_app
          ibmw_extensions_iceoryx_ext)
