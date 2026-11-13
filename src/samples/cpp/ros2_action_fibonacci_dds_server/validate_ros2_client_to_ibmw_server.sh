#!/bin/bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAMPLE_DIR="$SCRIPT_DIR"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd || true)}"
LOG_DIR="${LOG_DIR:-$SAMPLE_DIR/logs}"
mkdir -p "$LOG_DIR"

VALIDATION_ACTION="${VALIDATION_ACTION:-fibonacci}"
case "$VALIDATION_ACTION" in
  fibonacci)
    DEFAULT_SERVER_TARGET_NAME=ibmw_fibonacci_action_dds_server
    DEFAULT_CLIENT_TARGET_NAME=ibmw_fibonacci_action_ros2_client
    ;;
  gripper_command)
    DEFAULT_SERVER_TARGET_NAME=ibmw_action_dds_generic_gripper_command_server
    DEFAULT_CLIENT_TARGET_NAME=ibmw_gripper_command_action_ros2_client
    ;;
  *)
    echo "[FAIL] Unsupported VALIDATION_ACTION=$VALIDATION_ACTION; expected fibonacci or gripper_command" >&2
    exit 1
    ;;
esac
SERVER_TARGET_NAME="${SERVER_TARGET_NAME:-$DEFAULT_SERVER_TARGET_NAME}"
CLIENT_TARGET_NAME="${CLIENT_TARGET_NAME:-$DEFAULT_CLIENT_TARGET_NAME}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR:+$BUILD_DIR/$SERVER_TARGET_NAME}}"
CLIENT_BIN="${CLIENT_BIN:-${BUILD_DIR:+$BUILD_DIR/$CLIENT_TARGET_NAME}}"
if [ "$VALIDATION_ACTION" = "gripper_command" ]; then
  DEFAULT_ACTION_NAME=/gripper_command
  DEFAULT_ACTION_TYPE=control_msgs/action/GripperCommand
else
  DEFAULT_ACTION_NAME=/fibonacci
  DEFAULT_ACTION_TYPE=example_interfaces/action/Fibonacci
fi
ACTION_NAME="${ACTION_NAME:-$DEFAULT_ACTION_NAME}"
ACTION_TYPE="${ACTION_TYPE:-$DEFAULT_ACTION_TYPE}"
CLIENT_MODE="${CLIENT_MODE:-success}"
if [ "$#" -gt 0 ]; then
  CLIENT_MODE="$1"
fi
GOAL_ORDER="${GOAL_ORDER:-8}"
GRIPPER_POSITION="${GRIPPER_POSITION:-0.42}"
GRIPPER_EFFORT="${GRIPPER_EFFORT:-3.0}"
CANCEL_DELAY_MS="${CANCEL_DELAY_MS:-250}"
SERVER_READY_TIMEOUT_SECS="${SERVER_READY_TIMEOUT_SECS:-10}"
CLIENT_SERVER_WAIT_SECS="${CLIENT_SERVER_WAIT_SECS:-10}"
CLIENT_RESULT_TIMEOUT_SECS="${CLIENT_RESULT_TIMEOUT_SECS:-20}"
SERVER_LOG="$LOG_DIR/${SERVER_TARGET_NAME}.log"
CLIENT_LOG="$LOG_DIR/${CLIENT_TARGET_NAME}.log"
LDD_LOG="$LOG_DIR/${SERVER_TARGET_NAME}.ldd.log"
if [ -z "${SERVER_READY_MARKER:-}" ]; then
  case "$SERVER_TARGET_NAME" in
    ibmw_action_dds_generic_fibonacci_server|ibmw_action_dds_countdown_fibonacci_wire_server|ibmw_action_dds_generic_gripper_command_server)
      SERVER_READY_MARKER="DDS-only action server started for $ACTION_NAME"
      ;;
    *)
      SERVER_READY_MARKER="[PASS] DDS-only Fibonacci action server started for /fibonacci"
      ;;
  esac
fi

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
  if [ -f /opt/ros/${ROS_DISTRO:-humble}/setup.bash ]; then
    set +u
    source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
    set -u
    return 0
  fi
  echo "[FAIL] ROS2 environment is not sourced and /opt/ros/${ROS_DISTRO:-humble}/setup.bash is unavailable" >&2
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
    if grep -F "$SERVER_READY_MARKER" "$SERVER_LOG" >/dev/null 2>&1; then
      return 0
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "[FAIL] Server exited before emitting readiness marker" >&2
      if [ -f "$SERVER_LOG" ]; then
        echo "------- server log -------" >&2
        cat "$SERVER_LOG" >&2
      fi
      exit 1
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  echo "[FAIL] Timed out waiting for server readiness marker: $SERVER_READY_MARKER" >&2
  if [ -f "$SERVER_LOG" ]; then
    echo "------- server log -------" >&2
    cat "$SERVER_LOG" >&2
  fi
  exit 1
}

ensure_ros2_env
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export LD_LIBRARY_PATH="/usr/lib/$(uname -m)-linux-gnu:${LD_LIBRARY_PATH:-}"

echo "=========================================="
echo "ROS2 client -> IBMW DDS-only server validation"
echo "Sample dir: $SAMPLE_DIR"
echo "Build dir: ${BUILD_DIR:-<unset>}"
echo "Validation action: $VALIDATION_ACTION"
echo "Action: $ACTION_NAME [$ACTION_TYPE]"
echo "Mode: $CLIENT_MODE"
echo "RMW_IMPLEMENTATION: $RMW_IMPLEMENTATION"
echo "=========================================="

case "$CLIENT_MODE" in
  success|cancel|reject) ;;
  *)
    echo "[FAIL] Unsupported CLIENT_MODE=$CLIENT_MODE; expected success, cancel, or reject" >&2
    exit 1
    ;;
esac

[ -n "$SERVER_BIN" ] || fail_missing_binary "server" "<empty path>"
[ -n "$CLIENT_BIN" ] || fail_missing_binary "client" "<empty path>"
[ -x "$SERVER_BIN" ] || fail_missing_binary "server" "$SERVER_BIN"
[ -x "$CLIENT_BIN" ] || fail_missing_binary "client" "$CLIENT_BIN"

echo "[INFO] Checking server linkage guard: $SERVER_BIN"
ldd "$SERVER_BIN" | tee "$LDD_LOG"
if grep -E 'librcl(|cpp|cpp_action)|librmw|librcl_action' "$LDD_LOG" >/dev/null 2>&1; then
  echo "[FAIL] Server binary links against ROS2 client/runtime libraries. Expected DDS-only server." >&2
  exit 1
fi
echo "[PASS] Server linkage guard: no rcl/rclcpp/rclcpp_action/rmw dependencies detected"

: >"$SERVER_LOG"
: >"$CLIENT_LOG"

echo "[INFO] Starting server: $SERVER_BIN"
"$SERVER_BIN" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

wait_for_server_ready

echo "[INFO] Running native ROS2 action client: $CLIENT_BIN"
set +e
if [ "$VALIDATION_ACTION" = "gripper_command" ]; then
  "$CLIENT_BIN" \
    --mode "$CLIENT_MODE" \
    --position "$GRIPPER_POSITION" \
    --effort "$GRIPPER_EFFORT" \
    --cancel-delay-ms "$CANCEL_DELAY_MS" \
    --action-name "$ACTION_NAME" \
    --server-wait-seconds "$CLIENT_SERVER_WAIT_SECS" \
    --result-wait-seconds "$CLIENT_RESULT_TIMEOUT_SECS" \
    >"$CLIENT_LOG" 2>&1
else
  "$CLIENT_BIN" \
    --mode "$CLIENT_MODE" \
    --order "$GOAL_ORDER" \
    --cancel-delay-ms "$CANCEL_DELAY_MS" \
    --action-name "$ACTION_NAME" \
    --server-wait-seconds "$CLIENT_SERVER_WAIT_SECS" \
    --result-wait-seconds "$CLIENT_RESULT_TIMEOUT_SECS" \
    >"$CLIENT_LOG" 2>&1
fi
CLIENT_STATUS=$?
set -e

echo "------- client log -------"
cat "$CLIENT_LOG"
echo "------- server log -------"
cat "$SERVER_LOG"

if [ "$CLIENT_STATUS" -ne 0 ]; then
  echo "[FAIL] Native ROS2 client exited with status $CLIENT_STATUS" >&2
  exit "$CLIENT_STATUS"
fi

if ! grep -F "[PASS]" "$CLIENT_LOG" >/dev/null 2>&1; then
  echo "[FAIL] Client log did not contain a pass marker" >&2
  exit 1
fi

if [ "$CLIENT_MODE" = "reject" ]; then
  if ! grep -F "[INFO] Goal rejected" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected goal rejection marker" >&2
    exit 1
  fi
  if ! grep -F "[PASS] Reject case observed goal rejection" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected reject pass marker" >&2
    exit 1
  fi
else
  if ! grep -F "[INFO] Goal accepted" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected goal acceptance marker" >&2
    exit 1
  fi
fi

if [ "$CLIENT_MODE" = "success" ]; then
  if [ "$VALIDATION_ACTION" = "gripper_command" ]; then
    if ! grep -F "[PASS] Success case matched deterministic GripperCommand result" "$CLIENT_LOG" >/dev/null 2>&1; then
      echo "[FAIL] Client log did not contain the expected GripperCommand success pass marker" >&2
      exit 1
    fi

    if ! grep -F "[INFO] Feedback position=" "$CLIENT_LOG" >/dev/null 2>&1; then
      echo "[FAIL] Client log did not contain a GripperCommand feedback marker before success" >&2
      exit 1
    fi
  else
    EXPECTED_SEQUENCE="$(build_expected_sequence "$GOAL_ORDER")"
    if ! grep -F "Result sequence=$EXPECTED_SEQUENCE" "$CLIENT_LOG" >/dev/null 2>&1; then
      echo "[FAIL] Success case did not produce the expected Fibonacci sequence for order $GOAL_ORDER" >&2
      exit 1
    fi

    if ! grep -F "[PASS] Success case matched deterministic Fibonacci result" "$CLIENT_LOG" >/dev/null 2>&1; then
      echo "[FAIL] Client log did not contain the expected success pass marker" >&2
      exit 1
    fi

    if ! grep -F "[INFO] Feedback sequence=" "$CLIENT_LOG" >/dev/null 2>&1; then
      echo "[FAIL] Client log did not contain a feedback marker before success" >&2
      exit 1
    fi
  fi
elif [ "$CLIENT_MODE" = "cancel" ]; then
  if ! grep -F "goals_canceling=1" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected cancel acceptance marker" >&2
    exit 1
  fi

  if ! grep -F "[PASS] Cancel case reached terminal CANCELED status" "$CLIENT_LOG" >/dev/null 2>&1; then
    echo "[FAIL] Client log did not contain the expected cancel pass marker" >&2
    exit 1
  fi
fi

echo "[PASS] Validation harness completed successfully"
