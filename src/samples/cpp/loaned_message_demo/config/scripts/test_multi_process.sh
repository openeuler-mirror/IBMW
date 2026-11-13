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
source "/opt/ros/${ROS_DISTRO:-humble}/setup.bash" 2>/dev/null ||
  echo "warning: /opt/ros/${ROS_DISTRO:-humble}/setup.bash not sourced" >&2
set -u
export LD_LIBRARY_PATH="$BUILD_DIR:/usr/lib/$(uname -m)-linux-gnu:${LD_LIBRARY_PATH:-}"

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
    # Bounded wait for a clean exit, then SIGKILL whatever is left.
    local deadline=$((SECONDS + 3)) pid
    for pid in "${pids[@]}"; do
      while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do
        sleep 0.1
      done
      kill -9 "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    done
  fi
}
trap cleanup EXIT

# Logs carry ANSI colour codes; count matching lines of the stripped text.
# grep -c prints 0 and exits 1 on no match, so `|| true` (not `|| echo 0`).
count_lines() {
  local file="$1" pattern="$2"
  sed 's/\x1b\[[0-9;]*m//g' "$file" 2>/dev/null | grep -cE -- "$pattern" || true
}

fail() {
  echo "RESULT: FAILED - $1" >&2
  echo "--- publisher log tail ($PUB_LOG) ---" >&2
  tail -n 40 "$PUB_LOG" >&2 2>/dev/null || true
  echo "--- subscriber log tail ($SUB_LOG) ---" >&2
  tail -n 40 "$SUB_LOG" >&2 2>/dev/null || true
  exit 1
}

# Poll <file> every 0.1 s until it matches the ERE <pattern>. Fail (log tails,
# non-zero exit) if <pid> dies first or <timeout_sec> elapses.
wait_for_log() {
  local file="$1" pattern="$2" pid="$3" name="$4" timeout_sec="$5"
  local deadline=$((SECONDS + timeout_sec))
  until grep -qE -- "$pattern" "$file" 2>/dev/null; do
    if ! kill -0 "$pid" 2>/dev/null; then
      fail "$name exited before logging '$pattern'"
    fi
    if (( SECONDS >= deadline )); then
      fail "$name did not log '$pattern' within ${timeout_sec}s"
    fi
    sleep 0.1
  done
}

READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-15}"
# SubscriberModule logs [RECV] for messages 1-3 and every 5th after that.
MIN_RECV_LINES="${MIN_RECV_LINES:-3}"
ENGINE_RUNNING_PATTERN='Running( \(async\))?\. Waiting for shutdown signal\.'
PUBLISH_FAILURE_PATTERN='Failed to (borrow|publish) loaned message'

echo "=========================================="
echo "Multi-Process Communication Test"
echo "=========================================="
echo "Publisher: plain_pub_only_app (Point, LoanedMessage API)"
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
# The engine logs "Running..." once Start() has created every DDS reader.
wait_for_log "$SUB_LOG" "$ENGINE_RUNNING_PATTERN" "$SUB_PID" subscriber "$READY_TIMEOUT_SEC"

# Start publisher
echo "Starting publisher process..."
./plain_pub_only_app "$CFG_DIR/plain_pub_only_cfg.yaml" > "$PUB_LOG" 2>&1 &
PUB_PID=$!
wait_for_log "$PUB_LOG" "$ENGINE_RUNNING_PATTERN" "$PUB_PID" publisher "$READY_TIMEOUT_SEC"

# Poll until the subscriber has logged MIN_RECV_LINES samples; RUN_SECONDS is
# the deadline, not a fixed sleep.
echo "Waiting up to $RUN_SECONDS seconds for $MIN_RECV_LINES [RECV] lines..."
deadline=$((SECONDS + RUN_SECONDS))
while :; do
  PUB_FAILURES=$(count_lines "$PUB_LOG" "$PUBLISH_FAILURE_PATTERN")
  if [ "$PUB_FAILURES" -gt 0 ]; then
    fail "publisher logged $PUB_FAILURES loaned borrow/publish failures"
  fi
  SUB_COUNT=$(count_lines "$SUB_LOG" '\[RECV\]')
  if [ "$SUB_COUNT" -ge "$MIN_RECV_LINES" ]; then
    break
  fi
  kill -0 "$SUB_PID" 2>/dev/null || fail "subscriber exited while waiting for [RECV]"
  kill -0 "$PUB_PID" 2>/dev/null || fail "publisher exited while waiting for [RECV]"
  if (( SECONDS >= deadline )); then
    fail "subscriber logged $SUB_COUNT [RECV] lines in ${RUN_SECONDS}s (need $MIN_RECV_LINES)"
  fi
  sleep 0.2
done
PUB_COUNT=$(count_lines "$PUB_LOG" '\[SEND\]')

STRIP_ANSI='s/\x1b\[[0-9;]*m//g'

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="

echo ""
echo "--- Publisher output ---"
sed "$STRIP_ANSI" "$PUB_LOG" | grep -E "\[SEND\]|is_zero_copy|can_loan_messages|is_plain" | head -10 || true

echo ""
echo "--- Subscriber output ---"
sed "$STRIP_ANSI" "$SUB_LOG" | grep -E "\[RECV\]" | head -10 || true

echo ""
echo "=========================================="
echo "Publisher sent: $PUB_COUNT messages (sampled)"
echo "Subscriber received: $SUB_COUNT messages (sampled)"

if [ "$PUB_COUNT" -le 0 ]; then
  fail "no [SEND] evidence in the publisher log"
fi
echo "RESULT: PASSED - Cross-process DDS communication works!"
echo "=========================================="
