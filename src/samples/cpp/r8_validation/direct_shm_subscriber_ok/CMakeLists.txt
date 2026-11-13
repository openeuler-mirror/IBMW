# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# R8 validation sample (positive case, subscriber side), shm_ext counterpart
# of direct_iceoryx_subscriber_ok.
# See direct_shm_publisher_ok/CMakeLists.txt for the link-surface rationale.

add_executable(r8_validation_direct_shm_subscriber_ok
  standalone/main.cc)

target_link_libraries(r8_validation_direct_shm_subscriber_ok PRIVATE
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload)
