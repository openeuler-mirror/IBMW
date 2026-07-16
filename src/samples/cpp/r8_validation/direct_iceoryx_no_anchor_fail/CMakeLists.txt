# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DELIBERATELY UNANCHORED: this target's translation unit produces 1
# fresh R1 violation when scanned by check_red_lines.sh. Step T's
# baseline regen excludes this directory so the cumulative count stays
# at 0. Build is fine -- the file compiles and runs (returns 0); only
# the static R1 audit trips on it.

add_executable(r8_validation_direct_iceoryx_no_anchor_fail
  standalone/main.cc)

target_include_directories(r8_validation_direct_iceoryx_no_anchor_fail PRIVATE
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)

target_link_libraries(r8_validation_direct_iceoryx_no_anchor_fail PRIVATE
  ibmw::core::engine
  ibmw_extensions_iceoryx_ext
  iceoryx_posh::iceoryx_posh)
