#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Live ROS2 <-> IBMW wire-interop acceptance harness.

set -euo pipefail

SCRIPT_NAME=$(basename "$0")
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

ROS2_SETUP=${ROS2_SETUP:-/opt/ros/humble/setup.bash}
ROS_DOMAIN_ID=${ROS_DOMAIN_ID:-0}
LOG_DIR=${LOG_DIR:-/tmp/ibmw_ros2_live_interop_acceptance}
PUBSUB_WORKDIR=${PUBSUB_WORKDIR:-}
PUBSUB_PUB_CFG=${PUBSUB_PUB_CFG:-./cfg/ros2_pub_only_app.yaml}
PUBSUB_SUB_CFG=${PUBSUB_SUB_CFG:-./cfg/ros2_sub_only_app.yaml}
PUBSUB_TOPIC=${PUBSUB_TOPIC:-/chatter}
PUBSUB_TYPE=${PUBSUB_TYPE:-std_msgs/msg/String}
PUBSUB_ROS2_SUB_CMD=${PUBSUB_ROS2_SUB_CMD:-ros2 run demo_nodes_cpp listener}
PUBSUB_ROS2_PUB_CMD=${PUBSUB_ROS2_PUB_CMD:-ros2 run demo_nodes_cpp talker}
PUBSUB_ROS2_SUB_EXPECT=${PUBSUB_ROS2_SUB_EXPECT:-I heard:}
PUBSUB_IBMW_SUB_EXPECT=${PUBSUB_IBMW_SUB_EXPECT:-Received}
SERVICE_WORKDIR=${SERVICE_WORKDIR:-}
SERVICE_APP=${SERVICE_APP:-}
SERVICE_CFG=${SERVICE_CFG:-}
SERVICE_NAME=${SERVICE_NAME:-/add_two_ints}
SERVICE_TYPE=${SERVICE_TYPE:-example_interfaces/srv/AddTwoInts}

PIDS=()

usage() {
  cat <<USAGE
Usage:
  $SCRIPT_NAME pubsub --workdir <staged-ros2_interop_demo-dir> [--log-dir <dir>] [--pubsub-topic <topic>] [--pubsub-type <ros2-type>] [--ros2-sub-cmd <cmd>] [--ros2-pub-cmd <cmd>]
  $SCRIPT_NAME service --workdir <dir-with-dds-plugin> --app <demo_nodes_cpp_ibmw_app> --config <yaml> [--log-dir <dir>]
  $SCRIPT_NAME all --pubsub-workdir <dir> --service-workdir <dir> --service-app <app> --service-config <yaml> [--log-dir <dir>]

Environment defaults:
  ROS2_SETUP=$ROS2_SETUP
  ROS_DOMAIN_ID=$ROS_DOMAIN_ID
  LOG_DIR=$LOG_DIR

Pub/sub acceptance proves:
  1. IBMW publisher -> ROS2 subscriber command on PUBSUB_TOPIC/PUBSUB_TYPE.
  2. ROS2 publisher command -> IBMW subscriber on PUBSUB_TOPIC/PUBSUB_TYPE.

Pub/sub defaults preserve the original seed acceptance:
  PUBSUB_TOPIC=$PUBSUB_TOPIC
  PUBSUB_TYPE=$PUBSUB_TYPE
  PUBSUB_ROS2_SUB_CMD=$PUBSUB_ROS2_SUB_CMD
  PUBSUB_ROS2_PUB_CMD=$PUBSUB_ROS2_PUB_CMD

Service acceptance proves:
  1. IBMW AddTwoInts server is discoverable as $SERVICE_NAME $SERVICE_TYPE.
  2. stock ROS2 CLI service call returns sum=5 for a=2,b=3.

This harness intentionally checks received samples/responses, not discovery only.
USAGE
}

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -- -"$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
      sleep 1
    fi
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 -- -"$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

add_pid() {
  PIDS+=("$1")
}

require_file() {
  local path=$1
  [[ -f "$path" ]] || fail "required file not found: $path"
}

require_executable() {
  local path=$1
  [[ -x "$path" ]] || fail "required executable not found or not executable: $path"
}

source_ros2() {
  require_file "$ROS2_SETUP"
  # ROS setup scripts may reference unset shell variables.
  set +u
  # shellcheck disable=SC1090
  source "$ROS2_SETUP"
  set -u
  export ROS_DOMAIN_ID
  command -v ros2 >/dev/null 2>&1 || fail "ros2 CLI not found after sourcing $ROS2_SETUP"
}

prepare_logs() {
  mkdir -p "$LOG_DIR"
}

wait_for_process_alive() {
  local pid=$1
  local label=$2
  local log=$3
  sleep 3
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "----- $label log -----" >&2
    cat "$log" >&2 || true
    fail "$label exited before acceptance checks"
  fi
}

run_pubsub_acceptance() {
  [[ -n "$PUBSUB_WORKDIR" ]] || fail "pubsub workdir is required"
  require_executable "$PUBSUB_WORKDIR/ros2_pub_only_app"
  require_executable "$PUBSUB_WORKDIR/ros2_sub_only_app"
  require_file "$PUBSUB_WORKDIR/$PUBSUB_PUB_CFG"
  require_file "$PUBSUB_WORKDIR/$PUBSUB_SUB_CFG"

  local ibmw_pub_log="$LOG_DIR/ibmw_pub.log"
  local ros2_listener_log="$LOG_DIR/ros2_listener.log"
  local ibmw_sub_log="$LOG_DIR/ibmw_sub.log"
  local ros2_talker_log="$LOG_DIR/ros2_talker.log"
  local topics_log="$LOG_DIR/ros2_topic_list.log"
  local topic_info_log="$LOG_DIR/ros2_topic_info.log"

  echo "[pubsub] IBMW publisher -> ROS2 listener"
  setsid bash -c 'cd "$1" && exec ./ros2_pub_only_app "$2"' _ \
    "$PUBSUB_WORKDIR" "$PUBSUB_PUB_CFG" >"$ibmw_pub_log" 2>&1 &
  local ibmw_pub_pid=$!
  add_pid "$ibmw_pub_pid"
  wait_for_process_alive "$ibmw_pub_pid" "IBMW publisher" "$ibmw_pub_log"

  local discovered=no
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    timeout 15 ros2 topic list >"$topics_log" 2>&1 || {
      cat "$topics_log" >&2 || true
      fail "ros2 topic list failed during publisher discovery"
    }
    if grep -q "^${PUBSUB_TOPIC}$" "$topics_log"; then
      discovered=yes
      break
    fi
    sleep 1
  done
  [[ "$discovered" == yes ]] || {
    cat "$topics_log" >&2 || true
    fail "ROS2 did not discover ${PUBSUB_TOPIC} from IBMW publisher"
  }
  timeout 15 ros2 topic info "$PUBSUB_TOPIC" >"$topic_info_log" 2>&1 || {
    cat "$topic_info_log" >&2 || true
    fail "ros2 topic info ${PUBSUB_TOPIC} failed"
  }
  grep -q "Type: ${PUBSUB_TYPE}" "$topic_info_log" || {
    cat "$topic_info_log" >&2 || true
    fail "topic ${PUBSUB_TOPIC} did not report type ${PUBSUB_TYPE}"
  }

  local ros2_sub_received=no
  local attempt
  for attempt in 1 2 3; do
    timeout 20 bash -lc "$PUBSUB_ROS2_SUB_CMD" >"$ros2_listener_log" 2>&1 || true
    if grep -Eq "$PUBSUB_ROS2_SUB_EXPECT" "$ros2_listener_log"; then
      ros2_sub_received=yes
      break
    fi
    sleep 1
  done
  [[ "$ros2_sub_received" == yes ]] || {
    echo "----- ROS2 subscriber command log -----" >&2
    cat "$ros2_listener_log" >&2 || true
    fail "ROS2 subscriber command did not receive an IBMW ${PUBSUB_TOPIC} sample"
  }
  kill -- -"$ibmw_pub_pid" 2>/dev/null || kill "$ibmw_pub_pid" 2>/dev/null || true
  sleep 1
  if kill -0 "$ibmw_pub_pid" 2>/dev/null; then
    kill -9 -- -"$ibmw_pub_pid" 2>/dev/null || kill -9 "$ibmw_pub_pid" 2>/dev/null || true
  fi
  wait "$ibmw_pub_pid" 2>/dev/null || true

  echo "[pubsub] ROS2 talker -> IBMW subscriber"
  setsid bash -c 'cd "$1" && exec ./ros2_sub_only_app "$2"' _ \
    "$PUBSUB_WORKDIR" "$PUBSUB_SUB_CFG" >"$ibmw_sub_log" 2>&1 &
  local ibmw_sub_pid=$!
  add_pid "$ibmw_sub_pid"
  wait_for_process_alive "$ibmw_sub_pid" "IBMW subscriber" "$ibmw_sub_log"

  local sub_discovered=no
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    timeout 15 ros2 topic info "$PUBSUB_TOPIC" >"$topic_info_log" 2>&1 || true
    if grep -Eq '^Subscription count: [1-9][0-9]*$' "$topic_info_log"; then
      sub_discovered=yes
      break
    fi
    sleep 1
  done
  [[ "$sub_discovered" == yes ]] || {
    echo "----- ROS2 topic info log -----" >&2
    cat "$topic_info_log" >&2 || true
    fail "ROS2 graph did not discover the IBMW subscriber for ${PUBSUB_TOPIC}"
  }

  timeout 12 bash -lc "$PUBSUB_ROS2_PUB_CMD" >"$ros2_talker_log" 2>&1 || true
  grep -Eq "$PUBSUB_IBMW_SUB_EXPECT" "$ibmw_sub_log" || {
    echo "----- IBMW subscriber log -----" >&2
    cat "$ibmw_sub_log" >&2 || true
    echo "----- ROS2 publisher command log -----" >&2
    cat "$ros2_talker_log" >&2 || true
    fail "IBMW subscriber did not receive a ROS2 ${PUBSUB_TOPIC} sample"
  }
  kill -- -"$ibmw_sub_pid" 2>/dev/null || kill "$ibmw_sub_pid" 2>/dev/null || true
  sleep 1
  if kill -0 "$ibmw_sub_pid" 2>/dev/null; then
    kill -9 -- -"$ibmw_sub_pid" 2>/dev/null || kill -9 "$ibmw_sub_pid" 2>/dev/null || true
  fi
  wait "$ibmw_sub_pid" 2>/dev/null || true
  echo "[pubsub] PASS"
}

run_service_acceptance() {
  [[ -n "$SERVICE_WORKDIR" ]] || fail "service workdir is required"
  [[ -n "$SERVICE_APP" ]] || fail "service app is required"
  [[ -n "$SERVICE_CFG" ]] || fail "service config is required"
  require_executable "$SERVICE_APP"
  require_file "$SERVICE_CFG"

  local server_log="$LOG_DIR/ibmw_add_two_ints_server.log"
  local service_list_log="$LOG_DIR/ros2_service_list.log"
  local service_type_log="$LOG_DIR/ros2_service_type.log"
  local service_call_log="$LOG_DIR/ros2_service_call.log"

  echo "[service] IBMW AddTwoInts server -> ROS2 CLI client"
  (
    cd "$SERVICE_WORKDIR"
    "$SERVICE_APP" --mode=add-two-ints-server --run-seconds=20 --ibmw-config="$SERVICE_CFG"
  ) >"$server_log" 2>&1 &
  local server_pid=$!
  add_pid "$server_pid"
  wait_for_process_alive "$server_pid" "IBMW AddTwoInts server" "$server_log"

  local discovered=no
  for _ in 1 2 3 4 5; do
    timeout 10 ros2 service list >"$service_list_log" 2>&1 || true
    if grep -q "^${SERVICE_NAME}$" "$service_list_log"; then
      discovered=yes
      break
    fi
    sleep 2
  done
  [[ "$discovered" == yes ]] || {
    cat "$service_list_log" >&2 || true
    fail "ROS2 did not discover service $SERVICE_NAME"
  }

  timeout 10 ros2 service type "$SERVICE_NAME" >"$service_type_log" 2>&1 || {
    cat "$service_type_log" >&2 || true
    fail "ros2 service type $SERVICE_NAME failed"
  }
  grep -q "^${SERVICE_TYPE}$" "$service_type_log" || {
    cat "$service_type_log" >&2 || true
    fail "service $SERVICE_NAME did not report type $SERVICE_TYPE"
  }

  timeout 15 ros2 service call "$SERVICE_NAME" "$SERVICE_TYPE" '{a: 2, b: 3}' >"$service_call_log" 2>&1 || {
    cat "$service_call_log" >&2 || true
    fail "ros2 service call $SERVICE_NAME failed"
  }
  grep -q 'sum=5' "$service_call_log" || {
    cat "$service_call_log" >&2 || true
    fail "service call did not return sum=5"
  }
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  echo "[service] PASS"
}

MODE=${1:-}
[[ -n "$MODE" ]] || { usage; exit 2; }
shift || true

while [[ $# -gt 0 ]]; do
  case "$1" in
    --workdir)
      PUBSUB_WORKDIR=${2:-}
      SERVICE_WORKDIR=${2:-}
      shift 2
      ;;
    --pubsub-workdir)
      PUBSUB_WORKDIR=${2:-}
      shift 2
      ;;
    --service-workdir)
      SERVICE_WORKDIR=${2:-}
      shift 2
      ;;
    --app|--service-app)
      SERVICE_APP=${2:-}
      shift 2
      ;;
    --config|--service-config)
      SERVICE_CFG=${2:-}
      shift 2
      ;;
    --log-dir)
      LOG_DIR=${2:-}
      shift 2
      ;;
    --pubsub-topic)
      PUBSUB_TOPIC=${2:-}
      shift 2
      ;;
    --pubsub-type)
      PUBSUB_TYPE=${2:-}
      shift 2
      ;;
    --pubsub-pub-config)
      PUBSUB_PUB_CFG=${2:-}
      shift 2
      ;;
    --pubsub-sub-config)
      PUBSUB_SUB_CFG=${2:-}
      shift 2
      ;;
    --ros2-sub-cmd)
      PUBSUB_ROS2_SUB_CMD=${2:-}
      shift 2
      ;;
    --ros2-pub-cmd)
      PUBSUB_ROS2_PUB_CMD=${2:-}
      shift 2
      ;;
    --ros2-sub-expect)
      PUBSUB_ROS2_SUB_EXPECT=${2:-}
      shift 2
      ;;
    --ibmw-sub-expect)
      PUBSUB_IBMW_SUB_EXPECT=${2:-}
      shift 2
      ;;
    --ros2-setup)
      ROS2_SETUP=${2:-}
      shift 2
      ;;
    --domain-id)
      ROS_DOMAIN_ID=${2:-}
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "unknown argument: $1"
      ;;
  esac
done

case "$MODE" in
  pubsub|service|all)
    ;;
  *)
    usage
    exit 2
    ;;
esac

prepare_logs
source_ros2

echo "Live interop acceptance mode: $MODE"
echo "Logs: $LOG_DIR"
echo "ROS_DOMAIN_ID=$ROS_DOMAIN_ID"

case "$MODE" in
  pubsub)
    run_pubsub_acceptance
    ;;
  service)
    run_service_acceptance
    ;;
  all)
    run_pubsub_acceptance
    run_service_acceptance
    ;;
esac

echo "PASS: live ROS2 <-> IBMW acceptance checks completed"
