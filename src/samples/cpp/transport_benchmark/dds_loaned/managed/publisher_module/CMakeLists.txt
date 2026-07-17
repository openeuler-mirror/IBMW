# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})

add_library(${CUR_TARGET_NAME} STATIC publisher_module.cc)

target_include_directories(${CUR_TARGET_NAME}
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
         ${CMAKE_SOURCE_DIR}/src/extensions/transport)

# dds_loaned drives the FastDDS LoanedMessage zero-copy path through the
# Ros2StylePublisher<T> typed helper. We need:
#   - bench_payload  : the 5 Plain bench types + DDS type-name registration
#   - bench_common   : LoadBenchOptions / BenchCsvWriter / LatencyStats
#   - dds_types      : IBMW_DDS_REGISTER_CONTEXT_TYPE machinery
#   - ibmw_dds_extension (shared) : the dds driver itself, plus
#     create_publisher<T> / Ros2StylePublisher<T> headers/symbols.
target_link_libraries(${CUR_TARGET_NAME}
  PUBLIC ibmw::api::cpp
         ibmw::core::engine
         ibmw::samples::cpp::transport_benchmark::bench_payload
         ibmw::samples::cpp::transport_benchmark::bench_common
         ibmw::extensions::dds_types
         ibmw_dds_extension)
