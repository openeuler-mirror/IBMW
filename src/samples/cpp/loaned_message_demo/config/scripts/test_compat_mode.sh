#!/bin/bash
# test_compat_mode.sh - 测试 COMPAT 模式
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

source /opt/ros/humble/setup.bash 2>/dev/null || true
export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

echo "=========================================="
echo "Testing COMPAT Mode (30 seconds)"
echo "=========================================="
echo "Expected:"
echo "  Plain (Point): is_zero_copy=false (forced heap)"
echo "  NonPlain (String): is_zero_copy=false (heap)"
echo "  Zero-copy ratio: 0%"
echo "=========================================="

timeout 30 $BUILD_DIR/loaned_message_app $CFG_DIR/compat_mode_cfg.yaml 2>&1 || true

echo ""
echo "=========================================="
echo "COMPAT Mode Test Complete"
echo "=========================================="
