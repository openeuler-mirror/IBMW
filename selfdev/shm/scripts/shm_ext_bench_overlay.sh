#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Usage: shm_ext_bench_overlay.sh <repo> <tree>
# Makes a copied source tree build ibmw_shm_ext_bench: copies the bench source of
# <repo> and, when the tree's shm_ext/CMakeLists.txt has no bench block (a baseline
# commit), appends the block of <repo>.
set -euo pipefail
ext=src/extensions/transport/shm_ext
cp "$1/${ext}/test_peer/shm_ext_bench.cc" "$2/${ext}/test_peer/shm_ext_bench.cc"
grep -q 'BEGIN shm_ext bench block' "$2/${ext}/CMakeLists.txt" ||
  sed -n '/BEGIN shm_ext bench block/,/END shm_ext bench block/p' "$1/${ext}/CMakeLists.txt" >>"$2/${ext}/CMakeLists.txt"
