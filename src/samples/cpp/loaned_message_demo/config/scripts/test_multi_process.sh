#!/usr/bin/env bash
# test_multi_process.sh - 多进程通信测试
# 启动独立的 publisher 和 subscriber 进程，验证 DDS 跨进程通信
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="${CFG_DIR:-$SCRIPT_DIR/../cfg}"
BUILD_DIR="${BUILD_DIR:-$(pwd)}"
TMP_DIR="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-loaned-message-demo}"
RUN_SECONDS="${RUN_SECONDS:-10}"
SUB_LOG="$TMP_DIR/loaned_msg_sub.log"
PUB_LOG="$TMP_DIR/loaned_msg_pub.log"

set +u
source /opt/ros/humble/setup.bash 2>/dev/null || true
set -u
export LD_LIBRARY_PATH="$BUILD_DIR:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

mkdir -p "$TMP_DIR"
if [ ! -x "$BUILD_DIR/sub_only_app" ] || [ ! -x "$BUILD_DIR/plain_pub_only_app" ]; then
  echo "ERROR: missing sub_only_app or plain_pub_only_app under BUILD_DIR=$BUILD_DIR" >&2
  exit 1
fi
if [ ! -f "$CFG_DIR/sub_only_cfg.yaml" ] || [ ! -f "$CFG_DIR/plain_pub_only_cfg.yaml" ]; then
  echo "ERROR: missing loaned message cfg under CFG_DIR=$CFG_DIR" >&2
  exit 1
fi

cleanup() {
  local pids=()
  [ -n "${PUB_PID:-}" ] && pids+=("$PUB_PID")
  [ -n "${SUB_PID:-}" ] && pids+=("$SUB_PID")
  if [ "${#pids[@]}" -gt 0 ]; then
    kill "${pids[@]}" 2>/dev/null || true
    sleep 1
    kill -9 "${pids[@]}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=========================================="
echo "Multi-Process Communication Test"
echo "=========================================="
echo "Publisher: plain_pub_only_app (Point, zero-copy)"
echo "Subscriber: sub_only_app (separate process)"
echo "Build dir: $BUILD_DIR"
echo "Config dir: $CFG_DIR"
echo "=========================================="

# Start subscriber first (run from build dir so plugin path resolves)
echo ""
echo "Starting subscriber process..."
cd "$BUILD_DIR"
./sub_only_app "$CFG_DIR/sub_only_cfg.yaml" > "$SUB_LOG" 2>&1 &
SUB_PID=$!
sleep 3

# Start publisher
echo "Starting publisher process..."
./plain_pub_only_app "$CFG_DIR/plain_pub_only_cfg.yaml" > "$PUB_LOG" 2>&1 &
PUB_PID=$!

# Wait for messages to flow
echo "Waiting $RUN_SECONDS seconds for messages..."
sleep "$RUN_SECONDS"

# Check results - strip ANSI color codes from logs
STRIP_ANSI='s/\x1b\[[0-9;]*m//g'

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="

echo ""
echo "--- Publisher output ---"
sed "$STRIP_ANSI" "$PUB_LOG" | grep -E "\[SEND\]|is_zero_copy|Statistics|Zero-copy" | head -10 || echo "(no SEND messages found)"

echo ""
echo "--- Subscriber output ---"
sed "$STRIP_ANSI" "$SUB_LOG" | grep -E "\[RECV\]" | head -10 || echo "(no RECV messages found)"

# Verify
PUB_COUNT=$(sed "$STRIP_ANSI" "$PUB_LOG" | grep -c "\[SEND\]" 2>/dev/null || echo "0")
SUB_COUNT=$(sed "$STRIP_ANSI" "$SUB_LOG" | grep -c "\[RECV\]" 2>/dev/null || echo "0")

echo ""
echo "=========================================="
echo "Publisher sent: $PUB_COUNT messages (sampled)"
echo "Subscriber received: $SUB_COUNT messages (sampled)"

if [ "$SUB_COUNT" -gt 0 ]; then
  echo "RESULT: PASSED - Cross-process DDS communication works!"
else
  echo "RESULT: FAILED - Subscriber received no messages"
  echo "  Possible causes:"
  echo "  - DDS discovery takes time (try longer wait)"
  echo "  - Domain ID mismatch"
  echo "  - Network/firewall issues"
fi
echo "=========================================="

if [ "$PUB_COUNT" -le 0 ] || [ "$SUB_COUNT" -le 0 ]; then
  echo "ERROR: expected publisher and subscriber message evidence" >&2
  exit 1
fi
