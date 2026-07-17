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
# Parent samples/cpp/CMakeLists.txt wires this subdirectory in inside the
# IBMW_BUILD_ICEORYX_EXTENSION guard (Step P.cmake). The two _ok samples
# build under default --preset full; the two _fail samples are gated
# behind -DIBMW_BUILD_R8_FAIL_SAMPLES=ON (default OFF) and are exercised
# only by the Gate 9 dual-build matrix.

if(NOT IBMW_BUILD_ICEORYX_EXTENSION)
  message(STATUS "Skip r8_validation because IBMW_BUILD_ICEORYX_EXTENSION is OFF")
  return()
endif()

add_subdirectory(direct_iceoryx_publisher_ok)
add_subdirectory(direct_iceoryx_subscriber_ok)

if(IBMW_BUILD_R8_FAIL_SAMPLES)
  add_subdirectory(direct_iceoryx_mixed_api_fail)
  add_subdirectory(direct_iceoryx_no_anchor_fail)
endif()
