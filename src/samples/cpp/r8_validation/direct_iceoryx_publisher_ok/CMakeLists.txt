# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# R8 validation sample (positive case).
#
# Link surface is intentionally minimal -- no iceoryx_ext, no iceoryx_posh,
# no private TypeSupport include path. The sample asserts at compile time
# that publisher_for<T>() reaches a fully usable SenderAccess<T> through
# the public v2 facade alone, given a publicly-registered DDS type
# (BenchPlainPayload1KB from bench_payload, registered via
# IBMW_DDS_REGISTER_CONTEXT_TYPE in its umbrella header).
#
# bench_payload is INTERFACE and already INTERFACE-propagates dds_types,
# so we get fastcdr/fastrtps headers transitively without naming them here.

add_executable(r8_validation_direct_iceoryx_publisher_ok
  standalone/main.cc)

target_link_libraries(r8_validation_direct_iceoryx_publisher_ok PRIVATE
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload)
