#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR}"
SOURCE_CFG_DIR="${CFG_DIR:-$BUILD_DIR/play_smoke_cfg}"
CFG_DIR="$SOURCE_CFG_DIR"
BAGS_DIR="${BAGS_DIR:-$BUILD_DIR/play_smoke_bags}"
RUN_PARENT="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-play-smoke}"
IBMW_BIN="${IBMW_BIN:-$BUILD_DIR/ibmw}"
[[ -x "$IBMW_BIN" ]] || IBMW_BIN="$BUILD_DIR/bin/ibmw"
ROUDI_BIN="${ROUDI_BIN:-$BUILD_DIR/iox-roudi}"
[[ -x "$ROUDI_BIN" ]] || ROUDI_BIN="$BUILD_DIR/bin/iox-roudi"

# Serialize suites that use process-global middleware resources.  This keeps
# the fixed Iceoryx RouDi instance and external transport listeners from being
# shared by two smoke runs.  The descriptor is released automatically when
# this process exits.
LOCK_FILE="${PLAY_SMOKE_LOCK_FILE:-${TMPDIR:-/tmp}/ibmw-play-smoke.lock}"
exec 9>"$LOCK_FILE"
flock 9

set +u
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
set -u
export AMENT_PREFIX_PATH="$BUILD_DIR/play_smoke_ros2_runtime:${AMENT_PREFIX_PATH:-}"
export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/lib:$BUILD_DIR/play_smoke_ros2_runtime/lib:/usr/lib/$(uname -m)-linux-gnu:/opt/ros/${ROS_DISTRO:-humble}/lib:${LD_LIBRARY_PATH:-}"

# Shared-memory transports built into this tree. The play_smoke_test target
# writes one marker per built transport; a tree without either marker predates
# them and always required Iceoryx.
ICEORYX_BUILT=0
SHM_BUILT=0
[[ -f "$BUILD_DIR/play_smoke_iceoryx_required" ]] && ICEORYX_BUILT=1
[[ -f "$BUILD_DIR/play_smoke_shm_required" ]] && SHM_BUILT=1
[[ "$ICEORYX_BUILT" == 1 || "$SHM_BUILT" == 1 ]] || ICEORYX_BUILT=1

mkdir -p "$RUN_PARENT"
RUN_DIR="$(mktemp -d "$RUN_PARENT/run.XXXXXX")"
RUNTIME_DIR="$RUN_DIR/runtimes"
mkdir -m 700 -p "$RUNTIME_DIR"

RUN_TAG="$(basename "$RUN_DIR")"
DOMAIN_SEED="$(printf '%s' "$RUN_TAG" | cksum | awk '{print $1}')"
PLAY_SMOKE_DOMAIN_ID=$((100 + DOMAIN_SEED % 100))
export ROS_DOMAIN_ID="$PLAY_SMOKE_DOMAIN_ID"
MQTT_PORT=$((18885 + DOMAIN_SEED % 1000))
TCP_PORT=$((50281 + DOMAIN_SEED % 1000))
UDP_PORT=$((TCP_PORT + 1))
HTTP_PORT=$((TCP_PORT + 2))

prepare_run_configs() {
  [[ -d "$SOURCE_CFG_DIR" ]] || {
    echo "play smoke: missing config directory: $SOURCE_CFG_DIR" >&2
    exit 2
  }
  CFG_DIR="$RUN_DIR/cfg"
  mkdir -p "$CFG_DIR"
  for config in "$SOURCE_CFG_DIR"/*.yaml; do
    [[ -f "$config" ]] || continue
    sed \
      -e "s#/tmp/ibmw/runtimes#$RUNTIME_DIR#g" \
      -e "s/domain_id: 88/domain_id: $PLAY_SMOKE_DOMAIN_ID/g" \
      -e "s/18885/$MQTT_PORT/g" \
      -e "s/50281/$TCP_PORT/g" \
      -e "s/50282/$UDP_PORT/g" \
      -e "s/50283/$HTTP_PORT/g" \
      -e "s#\./cfg/tmp/#$RUN_DIR/cfg/tmp/#g" \
      -e "s#path: \.\./lib#path: $BUILD_DIR/lib#g" \
      -e "s#path: \./lib#path: $BUILD_DIR/lib#g" \
      -e "s#type_support_pkgs: \[\.\./lib#type_support_pkgs: [$BUILD_DIR/lib#g" \
      "$config" >"$CFG_DIR/$(basename "$config")"
  done
}
prepare_run_configs

CURRENT_CASE=startup
CURRENT_RUNTIME_LOG=""
CURRENT_RECEIVED=""
LAST_PLAY_LOG=""
RUNTIME_PID=""
PLAY_PID=""
ROUDI_PID=""
MOSQUITTO_PID=""
FAILED_CASES=()
SHM_ROOT_OWNED=""

stop_pid() {
  local pid="${1:-}" signal="${2:-INT}"
  [[ -n "$pid" ]] || return 0
  if kill -0 "$pid" 2>/dev/null; then
    kill -"$signal" "$pid" 2>/dev/null || true
    for _ in {1..50}; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

cleanup_runtime_files() {
  local runtime_id="$1"
  rm -f -- "$RUNTIME_DIR/v2/$runtime_id.json" \
    "$RUNTIME_DIR/v2/$runtime_id.sock" "$RUNTIME_DIR/v2/$runtime_id.lock"
}

cleanup() {
  trap - EXIT INT TERM
  stop_pid "$PLAY_PID"
  stop_pid "$RUNTIME_PID"
  stop_pid "$ROUDI_PID" TERM
  stop_pid "$MOSQUITTO_PID" TERM
  [[ -z "$SHM_ROOT_OWNED" ]] || rm -rf -- "$SHM_ROOT_OWNED"
}
trap cleanup EXIT INT TERM

# The shm transport needs no daemon. Unless the caller chose a root, give this
# run a private one so it never shares segments with another shm user. It is
# under /tmp, not $TMPDIR: root + "/ibmw_shm.<uid>.<domain>" must fit in 63 bytes.
if [[ "$SHM_BUILT" == 1 && -z "${IBMW_SHM_ROOT:-}" ]]; then
  IBMW_SHM_ROOT="$(mktemp -d /tmp/ibmw-ps.XXXXXX)"
  SHM_ROOT_OWNED="$IBMW_SHM_ROOT"
  export IBMW_SHM_ROOT
fi

case_cleanup() {
  trap - EXIT INT TERM
  stop_pid "$PLAY_PID"
  stop_pid "$RUNTIME_PID"
}

fail_case() {
  echo "FAIL: $CURRENT_CASE: $*" >&2
  for log in "$CURRENT_RUNTIME_LOG" "$LAST_PLAY_LOG"; do
    [[ -n "$log" && -f "$log" ]] || continue
    echo "--- $log ---" >&2
    sed -n '1,280p' "$log" >&2
  done
  [[ -n "$CURRENT_RECEIVED" && -f "$CURRENT_RECEIVED" ]] && {
    echo "--- received messages ---" >&2
    cat "$CURRENT_RECEIVED" >&2
  }
  echo "play smoke artifacts: $RUN_DIR" >&2
  exit 1
}

wait_for_log() {
  local file="$1" marker="$2" pid="$3"
  for _ in {1..200}; do
    grep -Fq "$marker" "$file" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

start_runtime() {
  local config="$1" runtime_id="${2:-}" ready_marker="${3:-Play smoke subscriber ready}"
  stop_pid "$RUNTIME_PID"
  RUNTIME_PID=""
  [[ -z "$runtime_id" ]] || cleanup_runtime_files "$runtime_id"
  CURRENT_RUNTIME_LOG="$RUN_DIR/$CURRENT_CASE.runtime.log"
  CURRENT_RECEIVED="$RUN_DIR/$CURRENT_CASE.received.log"
  PLAY_SMOKE_OUTPUT="$CURRENT_RECEIVED" \
    "$IBMW_BIN" run "$CFG_DIR/$config" >"$CURRENT_RUNTIME_LOG" 2>&1 &
  RUNTIME_PID=$!
  wait_for_log "$CURRENT_RUNTIME_LOG" "$ready_marker" "$RUNTIME_PID" ||
    fail_case "subscriber Runtime failed to become ready"
  if [[ -n "$runtime_id" ]]; then
    local registration="$RUNTIME_DIR/v2/$runtime_id.json"
    for _ in {1..100}; do
      [[ -s "$registration" ]] && return 0
      kill -0 "$RUNTIME_PID" 2>/dev/null || break
      sleep 0.1
    done
    fail_case "Runtime Agent '$runtime_id' did not register"
  fi
  # Settle time for transport matching with the player started next. Kept as
  # a sleep: the match happens with that later process, and the subscriber
  # logs no per-transport "matched" line that could be waited on here.
  sleep 0.5
}

stop_runtime() {
  stop_pid "$RUNTIME_PID"
  RUNTIME_PID=""
}

run_play() {
  LAST_PLAY_LOG="$RUN_DIR/$CURRENT_CASE.play.log"
  set +e
  # Normal playback cases are non-interactive.  Disconnect stdin so a caller
  # running this script through GNU timeout from a terminal cannot put bag
  # play's background process group into the stopped state (SIGTTIN/SIGTTOU).
  "$IBMW_BIN" bag play "$@" </dev/null >"$LAST_PLAY_LOG" 2>&1
  local status=$?
  set -e
  [[ "$status" == 0 ]] || fail_case "bag play exited with status $status"
}

start_play() {
  LAST_PLAY_LOG="$RUN_DIR/$CURRENT_CASE.play.log"
  "$IBMW_BIN" bag play "$@" </dev/null >"$LAST_PLAY_LOG" 2>&1 &
  PLAY_PID=$!
}

received_count() {
  local topic="${1:-}"
  [[ -f "$CURRENT_RECEIVED" ]] || { echo 0; return; }
  if [[ -z "$topic" ]]; then
    wc -l <"$CURRENT_RECEIVED"
  else
    awk -F'|' -v topic="$topic" '$1 == topic {count++} END {print count + 0}' \
      "$CURRENT_RECEIVED"
  fi
}

wait_for_count() {
  local topic="$1" expected="$2"
  for _ in {1..200}; do
    [[ "$(received_count "$topic")" -ge "$expected" ]] && return 0
    [[ -z "$PLAY_PID" ]] || kill -0 "$PLAY_PID" 2>/dev/null || true
    sleep 0.1
  done
  return 1
}

require_published() {
  grep -Fq "Published $1 message(s)." "$LAST_PLAY_LOG" ||
    fail_case "Published count is not $1"
}

require_published_at_least() {
  local minimum="$1"
  local published
  published="$(sed -nE 's/^Published ([0-9]+) message\(s\)\.$/\1/p' "$LAST_PLAY_LOG" | tail -1)"
  [[ "$published" =~ ^[0-9]+$ && "$published" -ge "$minimum" ]] ||
    fail_case "Published count is $published, expected at least $minimum"
}

require_topic_count() {
  local topic="$1" expected="$2" actual
  actual="$(received_count "$topic")"
  [[ "$actual" == "$expected" ]] ||
    fail_case "$topic received $actual messages, expected $expected"
}

require_topic_count_at_least() {
  local topic="$1" minimum="$2" actual
  actual="$(received_count "$topic")"
  [[ "$actual" =~ ^[0-9]+$ && "$actual" -ge "$minimum" ]] ||
    fail_case "$topic received $actual messages, expected at least $minimum"
}

require_field_sequence() {
  local topic="$1" field="$2" expected="$3" actual
  actual="$(awk -F'|' -v topic="$topic" -v field="$field" \
    '$1 == topic {values = values (values == "" ? "" : ",") $field} END {print values}' \
    "$CURRENT_RECEIVED")"
  [[ "$actual" == "$expected" ]] ||
    fail_case "$topic field $field is '$actual', expected '$expected'"
}

require_field_prefix() {
  local topic="$1" field="$2" limit="$3" expected="$4" actual
  actual="$(awk -F'|' -v topic="$topic" -v field="$field" -v limit="$limit" \
    '$1 == topic && count < limit {
       values = values (values == "" ? "" : ",") $field; count++
     } END {print values}' "$CURRENT_RECEIVED")"
  [[ "$actual" == "$expected" ]] ||
    fail_case "$topic first $limit values of field $field are '$actual', expected '$expected'"
}

require_cdr_data() {
  local topic="${1:-/ibmw_play_smoke/data}"
  require_topic_count "$topic" 10
  require_field_sequence "$topic" 2 "0,1,2,3,4,5,6,7,8,9"
  require_field_sequence "$topic" 3 "data-0,data-1,data-2,data-3,data-4,data-5,data-6,data-7,data-8,data-9"
}

require_cdr_data_prefix() {
  local topic="${1:-/ibmw_play_smoke/data}"
  require_topic_count_at_least "$topic" 10
  require_field_prefix "$topic" 2 10 "0,1,2,3,4,5,6,7,8,9"
  require_field_prefix "$topic" 3 10 \
    "data-0,data-1,data-2,data-3,data-4,data-5,data-6,data-7,data-8,data-9"
}

require_driver() {
  local topic="$1" expected="$2"
  awk -F'|' -v topic="$topic" -v expected="$expected" \
    '$1 == topic && $5 != expected {bad=1} END {exit bad}' \
    "$CURRENT_RECEIVED" || fail_case "$topic was not received through $expected"
}

require_serialization() {
  local topic="$1" expected="$2"
  awk -F'|' -v topic="$topic" -v expected="$expected" \
    '$1 == topic && $4 != expected {bad=1} END {exit bad}' \
    "$CURRENT_RECEIVED" || fail_case "$topic serialization is not $expected"
}

run_case() {
  local label="$1"
  shift
  case "$label" in
    transport_*) echo "play smoke: transport ${label#transport_}" ;;
    cli_*) echo "play smoke: feature ${label#cli_}" ;;
    *) echo "play smoke: feature $label" ;;
  esac
  set +e
  (set -Eeuo pipefail; trap case_cleanup EXIT INT TERM; "$@")
  local status=$?
  set -e
  if [[ "$status" != 0 ]]; then
    FAILED_CASES+=("$label")
    echo "play smoke: case failed: $label (status $status)" >&2
  fi
}

required=(
  "$IBMW_BIN"
  "$BAGS_DIR/cdr_mcap/metadata.yaml"
  "$BAGS_DIR/cdr_mcap_reader/metadata.yaml"
  "$BAGS_DIR/cdr_sqlite3/metadata.yaml"
  "$BAGS_DIR/ros2_mcap/metadata.yaml"
  "$BUILD_DIR/play_smoke_ros2_runtime/share/ament_index/resource_index/packages/example_ros2"
  "$BUILD_DIR/libplay_smoke_pkg.so" "$BUILD_DIR/libplay_smoke_ts.so")
[[ "$ICEORYX_BUILT" == 1 ]] && required+=("$ROUDI_BIN")
for artifact in "${required[@]}"; do
  [[ -e "$artifact" ]] || { echo "play smoke: missing $artifact" >&2; exit 2; }
done
required_configs=(
  play_smoke_local_subscriber.yaml play_smoke_local_player.yaml
  play_smoke_local_player_alt.yaml play_smoke_json_subscriber.yaml
  play_smoke_json_player.yaml play_smoke_mcap_reader.yaml
  play_smoke_sqlite_reader.yaml
  play_smoke_dds_subscriber.yaml play_smoke_dds_player.yaml
  play_smoke_mqtt_subscriber.yaml play_smoke_mqtt_player.yaml
  play_smoke_zenoh_subscriber.yaml play_smoke_zenoh_player.yaml
  play_smoke_ros2_subscriber.yaml play_smoke_ros2_player.yaml
  play_smoke_tcp_subscriber.yaml play_smoke_tcp_player.yaml
  play_smoke_udp_subscriber.yaml play_smoke_udp_player.yaml
  play_smoke_http_subscriber.yaml play_smoke_http_player.yaml)
[[ "$ICEORYX_BUILT" == 1 ]] &&
  required_configs+=(play_smoke_iceoryx_subscriber.yaml play_smoke_iceoryx_player.yaml)
[[ "$SHM_BUILT" == 1 ]] &&
  required_configs+=(play_smoke_shm_subscriber.yaml play_smoke_shm_player.yaml)
for config in "${required_configs[@]}"; do
  [[ -f "$CFG_DIR/$config" ]] || {
    echo "play smoke: missing $CFG_DIR/$config" >&2
    exit 2
  }
done
required_libraries=(
  libibmw_record_playback.so libibmw_dds_transport.so
  libibmw_mqtt_transport.so
  libibmw_zenoh_transport.so libibmw_ros2_transport.so
  libibmw_net_transport.so)
[[ "$ICEORYX_BUILT" == 1 ]] && required_libraries+=(libibmw_iceoryx_transport.so)
[[ "$SHM_BUILT" == 1 ]] && required_libraries+=(libibmw_shm_transport.so)
for library in "${required_libraries[@]}"; do
  [[ -f "$BUILD_DIR/$library" || -f "$BUILD_DIR/lib/$library" ]] || {
    echo "play smoke: missing $library" >&2
    exit 2
  }
done

cd "$BUILD_DIR"
for command in flock mosquitto mosquitto_pub ss python3; do
  command -v "$command" >/dev/null || { echo "play smoke: missing $command" >&2; exit 2; }
done

CURRENT_CASE=startup
if [[ "$ICEORYX_BUILT" == 1 ]] && ! pgrep -x iox-roudi >/dev/null 2>&1; then
  "$ROUDI_BIN" >"$RUN_DIR/iox-roudi.log" 2>&1 &
  ROUDI_PID=$!
  wait_for_log "$RUN_DIR/iox-roudi.log" "RouDi is ready for clients" "$ROUDI_PID" || {
    tail -n 20 "$RUN_DIR/iox-roudi.log" >&2
    fail_case "iox-roudi did not become ready"
  }
fi
if ! ss -ltn "( sport = :$MQTT_PORT )" 2>/dev/null | tail -n +2 | grep -q .; then
  mosquitto -p "$MQTT_PORT" -v >"$RUN_DIR/mosquitto.log" 2>&1 &
  MOSQUITTO_PID=$!
  mqtt_listening=0
  for _ in {1..100}; do
    kill -0 "$MOSQUITTO_PID" 2>/dev/null || break
    if ss -ltn "( sport = :$MQTT_PORT )" 2>/dev/null | tail -n +2 | grep -q .; then
      mqtt_listening=1
      break
    fi
    sleep 0.1
  done
  [[ "$mqtt_listening" == 1 ]] || {
    tail -n 20 "$RUN_DIR/mosquitto.log" >&2
    fail_case "Mosquitto did not listen on port $MQTT_PORT"
  }
fi
mosquitto_pub -h 127.0.0.1 -p "$MQTT_PORT" -t /ibmw_play_smoke/startup -m ready -q 1 ||
  fail_case "MQTT broker is unavailable"

run_local_basic() {
  CURRENT_CASE=transport_local
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --storage mcap \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 20
  require_cdr_data
  require_topic_count /ibmw_play_smoke/secondary 10
  require_field_sequence /ibmw_play_smoke/secondary 2 "100,101,102,103,104,105,106,107,108,109"
  require_driver /ibmw_play_smoke/data local
  stop_runtime
}

require_ros2_data() {
  require_topic_count /ibmw_play_smoke/ros2 10
  require_field_sequence /ibmw_play_smoke/ros2 2 "0,1,2,3,4,5,6,7,8,9"
  require_field_sequence /ibmw_play_smoke/ros2 3 \
    "ros2-0,ros2-1,ros2-2,ros2-3,ros2-4,ros2-5,ros2-6,ros2-7,ros2-8,ros2-9"
}

run_external_transport() {
  local driver="$1"
  CURRENT_CASE="transport_$driver"
  local bag="$BAGS_DIR/cdr_mcap"
  local topic=/ibmw_play_smoke/data
  local module_config="play_smoke_${driver}_subscriber.yaml"
  local player_config="$CFG_DIR/play_smoke_${driver}_player.yaml"
  if [[ "$driver" == ros2 ]]; then
    bag="$BAGS_DIR/ros2_mcap"
    topic=/ibmw_play_smoke/ros2
  fi
  start_runtime "$module_config"
  local args=("$bag" --topics "$topic" --config-file "$player_config")
  [[ "$driver" == dds ]] && args+=(--wait-for-all-acked 5.0)
  if [[ "$driver" == tcp || "$driver" == udp || "$driver" == http ]]; then
    # Net transport sends asynchronously.  Keep the player alive for the
    # subscriber's end-to-end assertion, then stop it cleanly; otherwise the
    # player's normal shutdown can race the final socket send.
    start_play "${args[@]}" --loop
    wait_for_count "$topic" 10 ||
      fail_case "$driver subscriber did not receive all 10 messages"
    kill -INT "$PLAY_PID"
    wait "$PLAY_PID" || fail_case "$driver loop player did not stop cleanly"
    PLAY_PID=""
    require_published_at_least 10
  else
    run_play "${args[@]}"
    require_published 10
    wait_for_count "$topic" 10 ||
      fail_case "$driver subscriber did not receive all 10 messages"
  fi
  if [[ "$driver" == ros2 ]]; then
    require_ros2_data
  elif [[ "$driver" == tcp || "$driver" == udp || "$driver" == http ]]; then
    # The loop may begin its next round before SIGINT is observed.  Validate
    # one complete round instead of treating that valid prefix as a failure.
    require_cdr_data_prefix
  else
    require_cdr_data
  fi
  grep -Fq "$topic -> Player-owned publisher" "$LAST_PLAY_LOG" ||
    fail_case "$driver playback did not use a Player-owned publisher"
  if [[ "$driver" == dds ]]; then
    grep -Fq "Raw DDS publisher registered" "$LAST_PLAY_LOG" ||
      fail_case "DDS raw publisher was not registered"
    grep -Fq "Using native DDS type for subscription '$topic'" \
      "$CURRENT_RUNTIME_LOG" || fail_case "DDS native subscriber was not registered"
    grep -Fq "Timed out waiting for Player publisher acknowledgments" \
      "$LAST_PLAY_LOG" && fail_case "DDS acknowledgments timed out"
  elif [[ "$driver" == ros2 ]]; then
    grep -Fq "ros2 driver register publish type for topic '$topic' success" \
      "$LAST_PLAY_LOG" || fail_case "ROS2 publisher was not registered"
    grep -Fq "subscribe topic '$topic' success" "$CURRENT_RUNTIME_LOG" ||
      fail_case "ROS2 subscriber was not registered"
  else
    require_driver "$topic" "$driver"
    require_serialization "$topic" cdr
  fi
  stop_runtime
}

run_storage_sqlite() {
  CURRENT_CASE=storage_sqlite3
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_sqlite3" --storage=sqlite3 \
    --config-file="$CFG_DIR/play_smoke_local_player.yaml"
  require_published 20
  require_cdr_data
  require_topic_count /ibmw_play_smoke/secondary 10
  stop_runtime
}

run_topics() {
  CURRENT_CASE=topics
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 10
  require_cdr_data
  require_topic_count /ibmw_play_smoke/secondary 0
  stop_runtime
}

run_remap() {
  CURRENT_CASE=remap
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --remap=/ibmw_play_smoke/data:=/ibmw_play_smoke/remapped \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 10
  require_cdr_data /ibmw_play_smoke/remapped
  require_topic_count /ibmw_play_smoke/data 0
  stop_runtime
}

run_queue() {
  CURRENT_CASE=read_ahead_queue
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --read-ahead-queue-size=1 --log-level debug \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 10
  require_cdr_data
  grep -Fq "Playback read-ahead queue: 1" "$LAST_PLAY_LOG" ||
    fail_case "debug log does not report queue size 1"
  stop_runtime
}

play_span_ns() {
  local topic="$1"
  awk -F'|' -v topic="$topic" '$1 == topic {if (!first) first=$6; last=$6} END {print last-first}' "$CURRENT_RECEIVED"
}

run_rate() {
  CURRENT_CASE=rate_1x
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data --rate 1 \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_cdr_data
  local span_1x
  span_1x="$(play_span_ns /ibmw_play_smoke/data)"
  stop_runtime

  CURRENT_CASE=rate_2x
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data -r 2 \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_cdr_data
  local span_2x
  span_2x="$(play_span_ns /ibmw_play_smoke/data)"
  (( span_1x > 700000000 && span_2x < span_1x * 3 / 4 )) ||
    fail_case "rate did not shorten playback: 1x=$span_1x ns, 2x=$span_2x ns"
  stop_runtime
}

run_delay() {
  CURRENT_CASE=delay
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  local started
  started="$(python3 -c 'import time; print(time.monotonic_ns())')"
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data -d 0.5 \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_cdr_data
  local first
  first="$(awk -F'|' '$1=="/ibmw_play_smoke/data" {print $6; exit}' "$CURRENT_RECEIVED")"
  (( first - started >= 400000000 )) ||
    fail_case "first message arrived before the configured delay"
  stop_runtime
}

run_offset() {
  CURRENT_CASE=start_offset
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --start-offset=0.4 --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 6
  require_topic_count /ibmw_play_smoke/data 6
  require_field_sequence /ibmw_play_smoke/data 2 "4,5,6,7,8,9"
  require_field_sequence /ibmw_play_smoke/data 3 "data-4,data-5,data-6,data-7,data-8,data-9"
  stop_runtime
}

run_paused() {
  CURRENT_CASE=start_paused
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  start_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    -p --disable-keyboard-controls \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  kill -0 "$PLAY_PID" 2>/dev/null ||
    fail_case "start-paused player exited before it could be resumed"
  sleep 0.4
  require_topic_count /ibmw_play_smoke/data 0
  kill -USR2 "$PLAY_PID"
  wait "$PLAY_PID" || fail_case "resumed player failed"
  PLAY_PID=""
  require_published 10
  require_cdr_data
  grep -Fq "Playback resumed." "$LAST_PLAY_LOG" || fail_case "resume marker missing"
  stop_runtime
}

run_disable_keyboard() {
  CURRENT_CASE=disable_keyboard
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  LAST_PLAY_LOG="$RUN_DIR/$CURRENT_CASE.play.log"
  python3 - "$IBMW_BIN" "$BAGS_DIR/cdr_mcap" \
    "$CFG_DIR/play_smoke_local_player.yaml" "$LAST_PLAY_LOG" <<'PY'
import os, pty, select, subprocess, sys, time
binary, bag, config, output = sys.argv[1:]
master, slave = pty.openpty()
process = subprocess.Popen(
    [binary, "bag", "play", bag, "--topics", "/ibmw_play_smoke/data",
     "--rate", "0.5", "--disable-keyboard-controls", "--config-file", config],
    stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)
data = bytearray()
deadline = time.monotonic() + 8
sent = False
while process.poll() is None and time.monotonic() < deadline:
    if not sent and time.monotonic() + 7 > deadline:
        os.write(master, b" ")
        sent = True
    ready, _, _ = select.select([master], [], [], 0.1)
    if ready:
        try: data.extend(os.read(master, 4096))
        except OSError: break
if process.poll() is None:
    process.kill()
    process.wait()
while True:
    try:
        chunk = os.read(master, 4096)
        if not chunk: break
        data.extend(chunk)
    except OSError:
        break
os.close(master)
open(output, "wb").write(data)
sys.exit(process.returncode)
PY
  require_published 10
  require_cdr_data
  grep -Fq "Playback paused." "$LAST_PLAY_LOG" &&
    fail_case "space paused playback even though keyboard controls were disabled"
  stop_runtime
}

run_loop() {
  CURRENT_CASE=loop
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  start_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data -l \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  wait_for_count /ibmw_play_smoke/data 20 || fail_case "two loops were not received"
  kill -INT "$PLAY_PID"
  wait "$PLAY_PID" || fail_case "loop player did not stop cleanly"
  PLAY_PID=""
  local count
  count="$(received_count /ibmw_play_smoke/data)"
  (( count >= 20 )) || fail_case "loop received only $count messages"
  local first_twenty
  first_twenty="$(awk -F'|' '$1 == "/ibmw_play_smoke/data" && count < 20 {
      values = values (values == "" ? "" : ",") $2; count++
    } END {print values}' "$CURRENT_RECEIVED")"
  [[ "$first_twenty" == "0,1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7,8,9" ]] ||
    fail_case "loop did not restart the sequence: $first_twenty"
  grep -Eq 'Published ([2-9][0-9]|[1-9][0-9]{2,}) message\(s\)\.' "$LAST_PLAY_LOG" ||
    fail_case "loop Published count is less than two rounds"
  stop_runtime
}

run_storage_config() {
  CURRENT_CASE=mcap_reader_config
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap_reader" --topics /ibmw_play_smoke/data \
    --storage-config-file "$CFG_DIR/play_smoke_mcap_reader.yaml" \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_published 3
  require_topic_count /ibmw_play_smoke/data 3
  require_field_sequence /ibmw_play_smoke/data 2 "0,2,1"
  require_field_sequence /ibmw_play_smoke/data 3 "data-0,data-2,data-1"
  stop_runtime

  CURRENT_CASE=sqlite_reader_config
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  local locked_bag="$RUN_DIR/sqlite_reader_bag"
  cp -a "$BAGS_DIR/cdr_sqlite3" "$locked_bag"
  local database
  database="$(find "$locked_bag" -maxdepth 1 -name '*.db3' -type f | head -1)"
  [[ -n "$database" ]] || fail_case "SQLite reader fixture has no database"
  local lock_ready="$RUN_DIR/sqlite-reader-lock.ready"
  python3 - "$database" "$lock_ready" >"$RUN_DIR/sqlite-reader-lock.log" 2>&1 <<'PY' &
import pathlib
import sqlite3
import sys
import time

database, ready = sys.argv[1:]
with sqlite3.connect(database, timeout=0) as connection:
    connection.execute("PRAGMA locking_mode=EXCLUSIVE")
    connection.execute("BEGIN EXCLUSIVE")
    pathlib.Path(ready).touch()
    time.sleep(0.4)
    connection.rollback()
PY
  local lock_pid=$!
  for _ in {1..100}; do
    [[ -f "$lock_ready" ]] && break
    kill -0 "$lock_pid" 2>/dev/null || fail_case "SQLite lock helper exited"
    sleep 0.01
  done
  [[ -f "$lock_ready" ]] || fail_case "SQLite lock helper did not acquire the lock"
  run_play "$locked_bag" --topics /ibmw_play_smoke/data \
    --storage-config-file="$CFG_DIR/play_smoke_sqlite_reader.yaml" \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  wait "$lock_pid" || fail_case "SQLite lock helper failed"
  require_cdr_data
  stop_runtime
}

run_config_file() {
  CURRENT_CASE=config_file
  local alt="$RUN_DIR/play_smoke_local_alt.yaml"
  sed 's/runtime_id: play-smoke-local/runtime_id: play-smoke-local-alt/' \
    "$CFG_DIR/play_smoke_local_subscriber.yaml" >"$alt"
  cp "$CFG_DIR/play_smoke_local_player_alt.yaml" "$RUN_DIR/player-alt.yaml"
  CURRENT_RUNTIME_LOG="$RUN_DIR/$CURRENT_CASE.runtime.log"
  CURRENT_RECEIVED="$RUN_DIR/$CURRENT_CASE.received.log"
  cleanup_runtime_files play-smoke-local-alt
  PLAY_SMOKE_OUTPUT="$CURRENT_RECEIVED" "$IBMW_BIN" run "$alt" >"$CURRENT_RUNTIME_LOG" 2>&1 &
  RUNTIME_PID=$!
  wait_for_log "$CURRENT_RUNTIME_LOG" "Play smoke subscriber ready" "$RUNTIME_PID" ||
    fail_case "alternate subscriber did not start"
  for _ in {1..100}; do
    [[ -s "$RUNTIME_DIR/v2/play-smoke-local-alt.json" ]] && break
    sleep 0.1
  done
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --config-file="$CFG_DIR/play_smoke_local_player_alt.yaml"
  require_cdr_data
  grep -Fq "Runtime play-smoke-local-alt embedded publisher" "$LAST_PLAY_LOG" ||
    fail_case "alternate config route was not selected"
  stop_runtime
}

run_transcode() {
  CURRENT_CASE=transcode_cdr_json
  start_runtime play_smoke_json_subscriber.yaml play-smoke-json
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data \
    --config-file "$CFG_DIR/play_smoke_json_player.yaml"
  require_published 10
  require_cdr_data
  require_serialization /ibmw_play_smoke/data json
  require_driver /ibmw_play_smoke/data local
  stop_runtime
}

run_help() {
  CURRENT_CASE=help
  LAST_PLAY_LOG="$RUN_DIR/help.log"
  "$IBMW_BIN" bag play --help >"$LAST_PLAY_LOG" 2>&1
  local options=(--storage --read-ahead-queue-size --rate --topics --loop
    --remap --delay --disable-keyboard-controls --start-paused --start-offset
    --config-file --storage-config-file --wait-for-all-acked --log-level)
  local option
  for option in "${options[@]}"; do
    grep -Fq -- "$option" "$LAST_PLAY_LOG" || fail_case "help is missing $option"
  done
}

run_log_level() {
  CURRENT_CASE=log_level_info
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data --log-level info \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_cdr_data
  grep -Fq "embedded publisher" "$LAST_PLAY_LOG" || fail_case "info route event missing"
  stop_runtime

  CURRENT_CASE=log_level_error
  start_runtime play_smoke_local_subscriber.yaml play-smoke-local
  run_play "$BAGS_DIR/cdr_mcap" --topics /ibmw_play_smoke/data --log-level error \
    --config-file "$CFG_DIR/play_smoke_local_player.yaml"
  require_cdr_data
  grep -Fq "embedded publisher" "$LAST_PLAY_LOG" &&
    fail_case "error level did not suppress info route event"
  stop_runtime
}

run_case transport_local run_local_basic
run_case transport_dds run_external_transport dds
if [[ "$ICEORYX_BUILT" == 1 ]]; then
  run_case transport_iceoryx run_external_transport iceoryx
fi
if [[ "$SHM_BUILT" == 1 ]]; then
  run_case transport_shm run_external_transport shm
fi
run_case transport_mqtt run_external_transport mqtt
run_case transport_zenoh run_external_transport zenoh
run_case transport_ros2 run_external_transport ros2
run_case transport_tcp run_external_transport tcp
run_case transport_udp run_external_transport udp
run_case transport_http run_external_transport http
run_case storage_sqlite3 run_storage_sqlite
run_case cli_help run_help
run_case cli_config_file run_config_file
run_case cli_topics run_topics
run_case cli_remap run_remap
run_case cli_read_ahead_queue run_queue
run_case cli_rate run_rate
run_case cli_delay run_delay
run_case cli_start_offset run_offset
run_case cli_start_paused run_paused
run_case cli_disable_keyboard run_disable_keyboard
run_case cli_loop run_loop
run_case cli_storage_config run_storage_config
run_case cli_log_level run_log_level
run_case playback_transcode run_transcode

if [[ "${#FAILED_CASES[@]}" -gt 0 ]]; then
  echo "FAIL: play smoke cases: ${FAILED_CASES[*]}" >&2
  echo "play smoke artifacts: $RUN_DIR" >&2
  exit 1
fi

echo "PASS: play smoke transport and CLI matrix"
echo "play smoke artifacts: $RUN_DIR"
