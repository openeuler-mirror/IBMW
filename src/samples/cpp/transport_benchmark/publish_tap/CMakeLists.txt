# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_executable(ibmw_publish_tap_benchmark publish_tap_benchmark.cc)
target_compile_features(ibmw_publish_tap_benchmark PRIVATE cxx_std_20)
target_link_libraries(ibmw_publish_tap_benchmark PRIVATE
  ibmw::core::engine
  ibmw_samples_cpp_transport_benchmark_bench_common)
