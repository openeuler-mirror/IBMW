#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAMPLE_DIR="$SCRIPT_DIR"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd || true)}"
LOG_DIR="${LOG_DIR:-$SAMPLE_DIR/logs}"
mkdir -p "$LOG_DIR"

SERVER_TARGET_NAME="${SERVER_TARGET_NAME:-ibmw_fibonacci_action_ros2_server}"
CLIENT_TARGET_NAME="${CLIENT_TARGET_NAME:-ibmw_fibonacci_action_dds_client}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR:+$BUILD_DIR/$SERVER_TARGET_NAME}}"
CLIENT_BIN="${CLIENT_BIN:-${BUILD_DIR:+$BUILD_DIR/$CLIENT_TARGET_NAME}}"
ACTION_NAME="${ACTION_NAME:-/fibonacci}"
CLIENT_MODE="${CLIENT_MODE:-success}"
if [ "$#" -gt 0 ]; then
  CLIENT_MODE="$1"
fi
GOAL_ORDER="${GOAL_ORDER:-8}"
FEEDBACK_DELAY_MS="${FEEDBACK_DELAY_MS:-50}"
SERVER_READY_TIMEOUT_SECS="${SERVER_READY_TIMEOUT_SECS:-10}"
CLIENT_RESULT_TIMEOUT_SECS="${CLIENT_RESULT_TIMEOUT_SECS:-10}"
CLIENT_WAIT_TIMEOUT_SECS="${CLIENT_WAIT_TIMEOUT_SECS:-10}"
SERVER_LOG="$LOG_DIR/ibmw_fibonacci_action_ros2_server.log"
CLIENT_LOG="$LOG_DIR/ibmw_fibonacci_action_dds_client.log"
LDD_LOG="$LOG_DIR/ibmw_fibonacci_action_dds_client.ldd.log"

build_expected_sequence() {
  local order="$1"
  local -a sequence=()
  local i

  for ((i = 0; i < order; ++i)); do
    if [ "$i" -eq 0 ]; then
      sequence+=(0)
    elif [ "$i" -eq 1 ]; then
      sequence+=(1)
    else
      sequence+=($((sequence[i - 1] + sequence[i - 2])))
    fi
  done

  local joined="["
  for ((i = 0; i < ${#sequence[@]}; ++i)); do
    if [ "$i" -ne 0 ]; then
      joined+=", "
    fi
    joined+="${sequence[i]}"
  done
  joined+="]"
  printf '%s' "$joined"
}

SERVER_PID=""

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

ensure_ros2_env() {
  if [ -n "${ROS_DISTRO:-}" ]; then
    return 0
  fi
  if [ -f /opt/ros/humble/setup.bash ]; then
    set +u
    source /opt/ros/humble/setup.bash
    set -u
    return 0
  fi
  echo "[FAIL] ROS2 environment is not sourced and /opt/ros/humble/setup.bash is unavailable" >&2
  exit 1
}

fail_missing_binary() {
  local label="$1"
  local path="$2"
  echo "[FAIL] Missing $label binary: $path" >&2
  echo "       Build or point $label via environment variables before running this harness." >&2
  exit 1
}

wait_for_server_ready() {
  local elapsed=0
  while [ "$elapsed" -lt "$SERVER_READY_TIMEOUT_SECS" ]; do
    if grep -F "SERVER_READY" "$SERVER_LOG" >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "[FAIL] Server exited before emitting SERVER_READY" >&2
      if [ -f "$SERVER_LOG" ]; then
        echo "------- server log -------" >&2
        cat "$SERVER_LOG" >&2
      fi
      exit 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  echo "[FAIL] Timed out waiting for SERVER_READY marker" >&2
  if [ -f "$SERVER_LOG" ]; then
    echo "------- server log -------" >&2
    cat "$SERVER_LOG" >&2
  fi
  exit 1
}

ensure_ros2_env
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

echo "=========================================="
echo "IBMW DDS-only client -> ROS2 server validation"
echo "Sample dir: $SAMPLE_DIR"
echo "Build dir: ${BUILD_DIR:-<unset>}"
echo "Action: $ACTION_NAME"
echo "Mode: $CLIENT_MODE"
echo "RMW_IMPLEMENTATION: $RMW_IMPLEMENTATION"
echo "=========================================="

case "$CLIENT_MODE" in
  success|cancel) ;;
  *)
    echo "[FAIL] Unsupported CLIENT_MODE=$CLIENT_MODE; expected success or cancel" >&2
    exit 1
    ;;
esac

[ -n "$SERVER_BIN" ] || fail_missing_binary "server" "<empty path>"
[ -n "$CLIENT_BIN" ] || fail_missing_binary "client" "<empty path>"
[ -x "$SERVER_BIN" ] || fail_missing_binary "server" "$SERVER_BIN"
[ -x "$CLIENT_BIN" ] || fail_missing_binary "client" "$CLIENT_BIN"

echo "[INFO] Checking client linkage guard: $CLIENT_BIN"
ldd "$CLIENT_BIN" | tee "$LDD_LOG"
if grep -E 'librcl(|cpp|cpp_action)|librmw|librcl_action' "$LDD_LOG" >/dev/null 2>&1; then
  echo "[FAIL] Client binary links against ROS2 runtime libraries. Expected DDS-only client." >&2
  exit 1
fi
echo "[PASS] Client linkage guard: no rcl/rclcpp/rclcpp_action/rmw dependencies detected"

: >"$SERVER_LOG"
: >"$CLIENT_LOG"

echo "[INFO] Starting native ROS2 server fixture: $SERVER_BIN"
"$SERVER_BIN" \
  --action-name "$ACTION_NAME" \
  --feedback-delay-ms "$FEEDBACK_DELAY_MS" \
  >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

wait_for_server_ready

echo "[INFO] Running DDS-only IBMW action client: $CLIENT_BIN"
set +e
CLIENT_RESULT_TIMEOUT_SECS="$CLIENT_RESULT_TIMEOUT_SECS" \
CLIENT_WAIT_TIMEOUT_SECS="$CLIENT_WAIT_TIMEOUT_SECS" \
"$CLIENT_BIN" \
  --mode "$CLIENT_MODE" \
  --order "$GOAL_ORDER" \
  --action-name "$ACTION_NAME" \
  >"$CLIENT_LOG" 2>&1
CLIENT_STATUS=$?
set -e

echo "------- client log -------"
cat "$CLIENT_LOG"
echo "------- server log -------"
cat "$SERVER_LOG"

if [ "$CLIENT_STATUS" -ne 0 ]; then
  echo "[FAIL] DDS-only IBMW client exited with status $CLIENT_STATUS" >&2
  exit "$CLIENT_STATUS"
fi

if ! grep -F "[PASS]" "$CLIENT_LOG" >/dev/null 2>&1; then
  echo "[FAIL] Client log did not contain a pass marker" >&2
  exit 1
fi

if ! grep -F "GOAL_ACCEPTED order=$GOAL_ORDER" "$SERVER_LOG" >/dev/null 2>&1; then
  echo "[FAIL] Server log did not contain the expected goal acceptance marker" >&2
  exit 1
fi

if [ "$CLIENT_MODE" = "success" ]; then
  EXPECTED_SEQUENCE="$(build_expected_sequence "$GOAL_ORDER")"
  if ! grep -F "Result sequence=$EXPECTED_SEQUENCE" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Success case did not produce the expected Fibonacci sequence for order $GOAL_ORDER" >&2
    exit 1
  fi

  if ! grep -F "GOAL_SUCCEEDED sequence=$EXPECTED_SEQUENCE" "$SERVER_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Server log did not contain the expected success sequence marker" >&2
    exit 1
  fi
else
  if ! grep -F "[PASS] Cancel case matched deterministic Fibonacci cancellation" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected cancel pass marker" >&2
    exit 1
  fi

  if ! grep -F "GOAL_CANCELING" "$SERVER_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Server log did not contain the expected canceling marker" >&2
    exit 1
  fi

  if ! grep -F "GOAL_CANCELED sequence=" "$SERVER_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Server log did not contain the expected canceled sequence marker" >&2
    exit 1
  fi
fi

echo "[PASS] Validation harness completed successfully"
