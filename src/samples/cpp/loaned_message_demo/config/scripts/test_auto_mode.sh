#!/bin/bash
# test_auto_mode.sh - 测试 AUTO 模式
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

source "/opt/ros/${ROS_DISTRO:-humble}/setup.bash" 2>/dev/null ||
  echo "warning: /opt/ros/${ROS_DISTRO:-humble}/setup.bash not sourced" >&2
export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

echo "=========================================="
echo "Testing AUTO Mode (30 seconds)"
echo "=========================================="
echo "Expected:"
echo "  Plain (Point): is_zero_copy=true (if SHM enabled)"
echo "  NonPlain (String): is_zero_copy=false, is_valid=true"
echo "=========================================="

timeout 30 $BUILD_DIR/loaned_message_app $CFG_DIR/loaned_message_cfg.yaml 2>&1 || true

echo ""
echo "=========================================="
echo "AUTO Mode Test Complete"
echo "=========================================="
