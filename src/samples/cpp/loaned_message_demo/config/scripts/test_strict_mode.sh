#!/bin/bash
# test_strict_mode.sh - 测试 STRICT 模式
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

source "/opt/ros/${ROS_DISTRO:-humble}/setup.bash" 2>/dev/null ||
  echo "warning: /opt/ros/${ROS_DISTRO:-humble}/setup.bash not sourced" >&2
export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

echo "=========================================="
echo "Testing STRICT Mode (30 seconds)"
echo "=========================================="
echo "Expected:"
echo "  Plain (Point): is_zero_copy=true (if SHM enabled)"
echo "  NonPlain (String): borrow rejected (is_valid=false)"
echo "=========================================="

timeout 30 $BUILD_DIR/loaned_message_app $CFG_DIR/strict_mode_cfg.yaml 2>&1 || true

echo ""
echo "=========================================="
echo "STRICT Mode Test Complete"
echo "=========================================="
