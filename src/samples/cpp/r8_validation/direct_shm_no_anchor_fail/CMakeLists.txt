# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DELIBERATELY UNANCHORED: this target's translation unit produces 1
# fresh R1 violation when scanned by check_red_lines.sh, like
# direct_iceoryx_no_anchor_fail. The baseline regen excludes this directory
# too. Build is fine -- the file compiles and runs (returns 0); only the
# static R1 audit trips on it.

add_executable(r8_validation_direct_shm_no_anchor_fail
  standalone/main.cc)

target_link_libraries(r8_validation_direct_shm_no_anchor_fail PRIVATE
  ibmw::core::engine
  ibmw_extensions_shm_ext)
