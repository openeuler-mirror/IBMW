# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# r8_validation: R8 / R8.2 compile-time validation samples.
#
# Four standalone samples that pin the v2 transport-factory API surface
# against the legacy iceoryx_ext direct-include surface:
#
#   direct_iceoryx_publisher_ok   -- v2 publisher_for<T>() only, no R-rule hits
#   direct_iceoryx_subscriber_ok  -- v2 subscriber_for<T>() only, no R-rule hits
#   direct_iceoryx_mixed_api_fail -- mixes both APIs, IbmwR82SlotMixCheck<>
#                                    static_assert trips at compile time
#                                    (gated by IBMW_BUILD_R8_FAIL_SAMPLES)
#   direct_iceoryx_no_anchor_fail -- includes iceoryx_ext header without
#                                    an IBMW_R1_EXEMPT anchor; documents
#                                    R1 escape-hatch convention enforced
#                                    by check_red_lines.sh
#                                    (gated by IBMW_BUILD_R8_FAIL_SAMPLES)
#
# The same four samples exist for the self-developed shm transport
# (direct_shm_*): the _ok pair is the identical facade-only TU, the _fail
# pair uses shm_ext/iox_style_publisher.h and the IbmwR82SlotMixCheck<>
# copy in shm_ext/shm_transport_driver.h.
#
# Parent samples/cpp/CMakeLists.txt wires this subdirectory in inside the
# IBMW_BUILD_BENCHMARKS guard (bench_payload). The direct_iceoryx_* samples
# need IBMW_BUILD_ICEORYX_EXTENSION, the direct_shm_* samples
# IBMW_BUILD_SHM_EXTENSION. The _ok samples build under default
# --preset full; the _fail samples are gated behind
# -DIBMW_BUILD_R8_FAIL_SAMPLES=ON (default OFF) and are exercised
# only by the Gate 9 dual-build matrix.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION AND NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip r8_validation because IBMW_BUILD_ICEORYX_EXTENSION and IBMW_BUILD_SHM_EXTENSION are OFF")
  return()
endif()

if(IBMW_BUILD_ICEORYX_EXTENSION)
  add_subdirectory(direct_iceoryx_publisher_ok)
  add_subdirectory(direct_iceoryx_subscriber_ok)

  if(IBMW_BUILD_R8_FAIL_SAMPLES)
    add_subdirectory(direct_iceoryx_mixed_api_fail)
    add_subdirectory(direct_iceoryx_no_anchor_fail)
  endif()
endif()

if(IBMW_BUILD_SHM_EXTENSION)
  add_subdirectory(direct_shm_publisher_ok)
  add_subdirectory(direct_shm_subscriber_ok)

  if(IBMW_BUILD_R8_FAIL_SAMPLES)
    add_subdirectory(direct_shm_mixed_api_fail)
    add_subdirectory(direct_shm_no_anchor_fail)
  endif()
endif()
