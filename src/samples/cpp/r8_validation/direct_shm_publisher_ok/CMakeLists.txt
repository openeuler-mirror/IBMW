# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# R8 validation sample (positive case), shm_ext counterpart of
# direct_iceoryx_publisher_ok.
#
# Link surface is intentionally minimal -- no shm_ext, no selfdev shm
# library, no private TypeSupport include path. The sample asserts at compile
# time that publisher_for<T>() reaches a fully usable SenderAccess<T> through
# the public v2 facade alone, in a build that has the shm transport and
# possibly no iceoryx. See direct_iceoryx_publisher_ok/CMakeLists.txt for the
# bench_payload rationale.

add_executable(r8_validation_direct_shm_publisher_ok
  standalone/main.cc)

target_link_libraries(r8_validation_direct_shm_publisher_ok PRIVATE
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload)
