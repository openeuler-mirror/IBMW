#!/bin/bash
# verify_demo.sh - 自动化验证脚本
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

source "/opt/ros/${ROS_DISTRO:-humble}/setup.bash" 2>/dev/null ||
  echo "warning: /opt/ros/${ROS_DISTRO:-humble}/setup.bash not sourced" >&2
export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

PASS=0
FAIL=0

check() {
  local name="$1"
  local result="$2"
  if [ "$result" = "0" ]; then
    echo "  [PASS] $name"
    PASS=$((PASS + 1))
  else
    echo "  [FAIL] $name"
    FAIL=$((FAIL + 1))
  fi
}

echo "=========================================="
echo "LoanedMessage Demo Verification"
echo "=========================================="

# Phase 1: Check executables
echo ""
echo "Phase 1: Executables"
for app in loaned_message_app config_test_app plain_pub_only_app nonplain_pub_only_app sub_only_app; do
  [ -f "$BUILD_DIR/$app" ]; check "$app" $?
done

# Phase 2: Config test
echo ""
echo "Phase 2: Config Tests"
OUTPUT=$(timeout 15 $BUILD_DIR/config_test_app $CFG_DIR/config_test_cfg.yaml 2>&1 || true)
echo "$OUTPUT" | grep -q "6/6 passed"; check "Config 6/6 passed" $?
echo "$OUTPUT" | grep -q "All tests passed"; check "All tests passed" $?

# Phase 3: Plain publisher
echo ""
echo "Phase 3: Plain Publisher"
OUTPUT=$(timeout 8 $BUILD_DIR/plain_pub_only_app $CFG_DIR/plain_pub_only_cfg.yaml 2>&1 || true)
echo "$OUTPUT" | grep -q "is_zero_copy=true"; check "Plain zero-copy" $?
echo "$OUTPUT" | grep -q "is_plain=true"; check "Plain type detected" $?

# Phase 4: End-to-end
echo ""
echo "Phase 4: End-to-End"
OUTPUT=$(timeout 8 $BUILD_DIR/loaned_message_app $CFG_DIR/loaned_message_cfg.yaml 2>&1 || true)
echo "$OUTPUT" | grep -q "\[RECV\] Point"; check "Point received" $?
echo "$OUTPUT" | grep -q "\[RECV\] String"; check "String received" $?
echo "$OUTPUT" | grep -q "is_zero_copy=false, is_plain=false"; check "NonPlain fallback" $?

echo ""
echo "=========================================="
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
  echo "All verifications passed!"
else
  echo "$FAIL verification(s) failed"
fi
echo "=========================================="

exit $FAIL
