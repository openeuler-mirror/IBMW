#!/bin/bash
# run_all_tests.sh - 运行所有 LoanedMessage Demo 测试
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

if [ ! -d "$BUILD_DIR" ]; then
  echo "Error: build directory not found. Set BUILD_DIR env var."
  exit 1
fi

source /opt/ros/humble/setup.bash 2>/dev/null || true
export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

echo "=========================================="
echo "LoanedMessage Demo - All Tests"
echo "Build dir: $BUILD_DIR"
echo "=========================================="

# Test 1: Config mode tests (6 scenarios)
echo ""
echo "--- Test 1: Configuration Mode Tests ---"
timeout 15 $BUILD_DIR/config_test_app $CFG_DIR/config_test_cfg.yaml 2>&1 | grep -E "PASSED|FAILED|Summary|All tests" || true
echo "--- Test 1 complete ---"

# Test 2: Plain publisher (AUTO mode, 5 seconds)
echo ""
echo "--- Test 2: Plain Publisher (AUTO mode) ---"
timeout 5 $BUILD_DIR/plain_pub_only_app $CFG_DIR/plain_pub_only_cfg.yaml 2>&1 | grep -E "\[SEND\]|is_zero_copy|can_loan|Statistics|Zero-copy" | head -10 || true
echo "--- Test 2 complete ---"

# Test 3: NonPlain publisher (AUTO mode, 5 seconds)
echo ""
echo "--- Test 3: NonPlain Publisher (AUTO mode) ---"
timeout 5 $BUILD_DIR/nonplain_pub_only_app $CFG_DIR/nonplain_pub_only_cfg.yaml 2>&1 | grep -E "\[SEND\]|borrow|is_zero_copy|Statistics|Fallback" | head -10 || true
echo "--- Test 3 complete ---"

# Test 4: Full end-to-end (AUTO mode, 10 seconds)
echo ""
echo "--- Test 4: Full End-to-End (AUTO mode) ---"
timeout 10 $BUILD_DIR/loaned_message_app $CFG_DIR/loaned_message_cfg.yaml 2>&1 | grep -E "\[SEND\]|\[RECV\]|Statistics|Zero-copy" | head -20 || true
echo "--- Test 4 complete ---"

# Test 5: STRICT mode (10 seconds)
echo ""
echo "--- Test 5: Full End-to-End (STRICT mode) ---"
timeout 10 $BUILD_DIR/loaned_message_app $CFG_DIR/strict_mode_cfg.yaml 2>&1 | grep -E "\[SEND\]|\[RECV\]|borrow|rejected|Statistics" | head -20 || true
echo "--- Test 5 complete ---"

# Test 6: COMPAT mode (10 seconds)
echo ""
echo "--- Test 6: Full End-to-End (COMPAT mode) ---"
timeout 10 $BUILD_DIR/loaned_message_app $CFG_DIR/compat_mode_cfg.yaml 2>&1 | grep -E "\[SEND\]|\[RECV\]|Statistics|Zero-copy" | head -20 || true
echo "--- Test 6 complete ---"

echo ""
echo "=========================================="
echo "All tests complete!"
echo "=========================================="
