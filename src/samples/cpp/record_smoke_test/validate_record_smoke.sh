#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR}"
CFG_DIR="${CFG_DIR:-$BUILD_DIR/cfg}"
RUN_PARENT="${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-record-smoke}"
RUNTIME_DIR="${XDG_RUNTIME_DIR:+${XDG_RUNTIME_DIR}/ibmw}"
RUNTIME_DIR="${RUNTIME_DIR:-/tmp/ibmw-$(id -u)}"
IBMW_BIN="${IBMW_BIN:-$BUILD_DIR/ibmw}"
[[ -x "$IBMW_BIN" ]] || IBMW_BIN="$BUILD_DIR/bin/ibmw"
ROUDI_BIN="${ROUDI_BIN:-$BUILD_DIR/bin/iox-roudi}"
[[ -x "$ROUDI_BIN" ]] || ROUDI_BIN="$BUILD_DIR/iox-roudi"

set +u
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
set -u
export ROS_DOMAIN_ID=87
export AMENT_PREFIX_PATH="$BUILD_DIR/record_smoke_ros2_runtime:${AMENT_PREFIX_PATH:-}"
export LD_LIBRARY_PATH="$BUILD_DIR:$BUILD_DIR/lib:$BUILD_DIR/record_smoke_ros2_runtime/lib:/usr/lib/$(uname -m)-linux-gnu:/opt/ros/${ROS_DISTRO:-humble}/lib:${LD_LIBRARY_PATH:-}"

# Shared-memory transports built into this tree. The record_smoke_test target
# writes one marker per built transport; a tree without either marker predates
# them and always required Iceoryx.
ICEORYX_BUILT=0
SHM_BUILT=0
[[ -f "$BUILD_DIR/record_smoke_iceoryx_required" ]] && ICEORYX_BUILT=1
[[ -f "$BUILD_DIR/record_smoke_shm_required" ]] && SHM_BUILT=1
[[ "$ICEORYX_BUILT" == 1 || "$SHM_BUILT" == 1 ]] || ICEORYX_BUILT=1

required_configs=(
  record_smoke_local.yaml
  record_smoke_dds.yaml
  record_smoke_mqtt.yaml
  record_smoke_zenoh.yaml
  record_smoke_ros2.yaml
  record_smoke_tcp.yaml
  record_smoke_udp.yaml
  record_smoke_http.yaml
  record_smoke_features.yaml
  record_smoke_json.yaml
  record_smoke_sim_time.yaml
  record_smoke_late.yaml
  record_smoke_mcap_storage.yaml
  record_smoke_sqlite_storage.yaml
  record_smoke_transcode.yaml
)
required_artifacts=(
  librecord_smoke_pkg.so
  librecord_smoke_ts.so
  libcommon_dds_ts.so
  libibmw_record_playback.so
  libibmw_dds_transport.so
  libibmw_mqtt_transport.so
  libibmw_zenoh_transport.so
  libibmw_ros2_transport.so
  libibmw_net_transport.so
)
required_commands=(jq mosquitto mosquitto_pub python3 ss stdbuf timeout)
if [[ "$ICEORYX_BUILT" == 1 ]]; then
  required_configs+=(record_smoke_iceoryx.yaml)
  required_artifacts+=(libibmw_iceoryx_transport.so)
fi
if [[ "$SHM_BUILT" == 1 ]]; then
  required_configs+=(record_smoke_shm.yaml)
  required_artifacts+=(libibmw_shm_transport.so)
fi

[[ -x "$IBMW_BIN" ]] || {
  echo "record smoke: missing ibmw binary: $IBMW_BIN" >&2
  exit 2
}
if [[ "$ICEORYX_BUILT" == 1 ]]; then
  [[ -x "$ROUDI_BIN" ]] || {
    echo "record smoke: missing iox-roudi binary: $ROUDI_BIN" >&2
    exit 2
  }
fi
for item in "${required_configs[@]}"; do
  [[ -f "$CFG_DIR/$item" ]] || {
    echo "record smoke: missing config: $CFG_DIR/$item" >&2
    exit 2
  }
done
for item in "${required_artifacts[@]}"; do
  [[ -f "$BUILD_DIR/$item" || -f "$BUILD_DIR/lib/$item" ]] || {
    echo "record smoke: missing artifact: $item" >&2
    exit 2
  }
done
for item in "${required_commands[@]}"; do
  command -v "$item" >/dev/null || {
    echo "record smoke: missing command: $item" >&2
    exit 2
  }
done

mkdir -p "$RUN_PARENT"
RUN_DIR="$(mktemp -d "$RUN_PARENT/run.XXXXXX")"
cd "$BUILD_DIR"

SHM_ROOT_OWNED=""

RUNTIME_PID=""
RECORD_PID=""
ROUDI_PID=""
MOSQUITTO_PID=""
CURRENT_CASE="startup"
CURRENT_RUNTIME_LOG=""
LAST_RECORD_LOG=""
LAST_INFO=""
LAST_OUT=""
CASE_CHILD=0
FAILED_CASES=()

stop_pid() {
  local pid="${1:-}"
  local signal="${2:-INT}"
  [[ -n "$pid" ]] || return 0
  if kill -0 "$pid" 2>/dev/null; then
    kill -"$signal" "$pid" 2>/dev/null || true
    for _ in {1..40}; do
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
    "$RUNTIME_DIR/v2/$runtime_id.sock" \
    "$RUNTIME_DIR/v2/$runtime_id.lock"
}

cleanup() {
  trap - EXIT INT TERM
  stop_pid "$RECORD_PID"
  stop_pid "$RUNTIME_PID"
  if [[ "$CASE_CHILD" == 0 ]]; then
    stop_pid "$ROUDI_PID" TERM
    stop_pid "$MOSQUITTO_PID" TERM
    [[ -z "$SHM_ROOT_OWNED" ]] || rm -rf -- "$SHM_ROOT_OWNED"
  fi
}
trap cleanup EXIT INT TERM

# The shm transport needs no daemon. Unless the caller chose a root, give this
# run a private one so it never shares segments with another shm user. It is
# under /tmp, not $TMPDIR: root + "/ibmw_shm.<uid>.<domain>" must fit in 63 bytes.
if [[ "$SHM_BUILT" == 1 && -z "${IBMW_SHM_ROOT:-}" ]]; then
  IBMW_SHM_ROOT="$(mktemp -d /tmp/ibmw-rs.XXXXXX)"
  SHM_ROOT_OWNED="$IBMW_SHM_ROOT"
  export IBMW_SHM_ROOT
fi

fail_case() {
  echo "FAIL: $CURRENT_CASE: $*" >&2
  [[ -f "$CURRENT_RUNTIME_LOG" ]] && {
    echo "--- runtime log ---" >&2
    sed -n '1,260p' "$CURRENT_RUNTIME_LOG" >&2
  }
  [[ -f "$LAST_RECORD_LOG" ]] && {
    echo "--- record log ---" >&2
    sed -n '1,260p' "$LAST_RECORD_LOG" >&2
  }
  [[ -f "$LAST_INFO" ]] && {
    echo "--- info log ---" >&2
    sed -n '1,260p' "$LAST_INFO" >&2
  }
  echo "record smoke artifacts: $RUN_DIR" >&2
  exit 1
}

wait_for_log() {
  local log_file="$1"
  local marker="$2"
  local pid="$3"
  for _ in {1..150}; do
    grep -Fq "$marker" "$log_file" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

start_services() {
  if [[ "$ICEORYX_BUILT" == 1 ]] && ! pgrep -x iox-roudi >/dev/null 2>&1; then
    "$ROUDI_BIN" >"$RUN_DIR/iox-roudi.log" 2>&1 &
    ROUDI_PID=$!
    wait_for_log "$RUN_DIR/iox-roudi.log" "RouDi is ready for clients" "$ROUDI_PID" || {
      tail -n 20 "$RUN_DIR/iox-roudi.log" >&2
      fail_case "iox-roudi did not become ready"
    }
  fi

  if ! ss -ltn '( sport = :18884 )' 2>/dev/null | tail -n +2 | grep -q .; then
    mosquitto -p 18884 -v >"$RUN_DIR/mosquitto.log" 2>&1 &
    MOSQUITTO_PID=$!
    local listening=0
    for _ in {1..100}; do
      kill -0 "$MOSQUITTO_PID" 2>/dev/null || break
      if ss -ltn '( sport = :18884 )' 2>/dev/null | tail -n +2 | grep -q .; then
        listening=1
        break
      fi
      sleep 0.1
    done
    [[ "$listening" == 1 ]] || {
      tail -n 20 "$RUN_DIR/mosquitto.log" >&2
      fail_case "Mosquitto did not listen on port 18884"
    }
  fi
  mosquitto_pub -h 127.0.0.1 -p 18884 \
    -t /ibmw_record_smoke/startup -m ready -q 1 ||
    fail_case "port 18884 is not a usable MQTT broker"
}

start_runtime() {
  local cfg_name="$1"
  local runtime_id="$2"
  local expected_driver="${3:-}"
  local expected_topic="${4:-}"
  local expected_mode="${5:-}"
  stop_pid "$RUNTIME_PID"
  RUNTIME_PID=""
  cleanup_runtime_files "$runtime_id"
  CURRENT_RUNTIME_LOG="$RUN_DIR/${CURRENT_CASE}.runtime.log"
  "$IBMW_BIN" run "$CFG_DIR/$cfg_name" >"$CURRENT_RUNTIME_LOG" 2>&1 &
  RUNTIME_PID=$!

  local registration="$RUNTIME_DIR/v2/$runtime_id.json"
  for _ in {1..150}; do
    if [[ -s "$registration" ]] &&
       jq -e --argjson pid "$RUNTIME_PID" '.pid == $pid' \
         "$registration" >/dev/null 2>&1 &&
       grep -Fq "Record smoke module ready" "$CURRENT_RUNTIME_LOG"; then
      if [[ -n "$expected_driver" ]]; then
        jq -e \
          --arg topic "$expected_topic" \
          --arg driver "$expected_driver" \
          --arg mode "$expected_mode" \
          'any(.topics[];
             .topic == $topic and .published == true and
             (.available_drivers | index($driver)) != null and
             .capture_capabilities[$driver].mode == $mode)' \
          "$registration" >/dev/null ||
          fail_case "registration does not advertise $expected_driver/$expected_mode for $expected_topic"
      fi
      return 0
    fi
    kill -0 "$RUNTIME_PID" 2>/dev/null ||
      fail_case "runtime exited before registration: $cfg_name"
    sleep 0.1
  done
  fail_case "runtime did not register: $cfg_name"
}

stop_runtime() {
  local status=0
  if [[ -n "$RUNTIME_PID" ]]; then
    set +e
    kill -INT "$RUNTIME_PID" 2>/dev/null
    wait "$RUNTIME_PID"
    status=$?
    set -e
    [[ "$status" == 0 || "$status" == 130 ]] ||
      fail_case "runtime exited with status $status"
    RUNTIME_PID=""
  fi
}

start_record() {
  LAST_RECORD_LOG="$RUN_DIR/${CURRENT_CASE}.record.log"
  LAST_INFO=""
  LAST_OUT="$RUN_DIR/${CURRENT_CASE}.bag"
  # Line-buffer stdout so the recorder's progress lines can be waited on.
  stdbuf -oL "$IBMW_BIN" bag record "$@" --output "$LAST_OUT" \
    >"$LAST_RECORD_LOG" 2>&1 &
  RECORD_PID=$!
  # Printed after the initial discovery query and storage setup.
  wait_for_log "$LAST_RECORD_LOG" "Recording to '$LAST_OUT'" "$RECORD_PID" ||
    fail_case "recorder did not start recording"
  [[ -d "$LAST_OUT" ]] || fail_case "recorder did not create output"
}

# Wait until the recorder subscribed to a topic, then record for a fixed
# window: the publishers run at 10 Hz and the checks need at least 5 messages.
record_after_start() {
  local topic="$1"
  wait_for_log "$LAST_RECORD_LOG" "  started $topic -> " "$RECORD_PID" ||
    fail_case "recorder did not start capturing $topic"
  sleep 1.0
}

stop_record() {
  local status=0
  if [[ -n "$RECORD_PID" ]]; then
    set +e
    kill -INT "$RECORD_PID" 2>/dev/null
    wait "$RECORD_PID"
    status=$?
    set -e
    [[ "$status" == 0 ]] || fail_case "recorder exited with status $status"
    RECORD_PID=""
  fi
}

read_info() {
  LAST_INFO="$RUN_DIR/${CURRENT_CASE}.info.log"
  "$IBMW_BIN" bag info "$LAST_OUT" >"$LAST_INFO" 2>&1 ||
    fail_case "bag info failed"
}

info_value() {
  local label="$1"
  awk -F: -v label="$label" '$1 == label {sub(/^[[:space:]]+/, "", $2); print $2; exit}' "$LAST_INFO"
}

message_count() {
  info_value Messages
}

file_count() {
  info_value Files
}

topic_message_count() {
  local topic="$1"
  awk -v topic="$topic" '
    $0 == "  Topic: " topic {found=1; next}
    found && /^    Messages:/ {print $2; exit}
  ' "$LAST_INFO"
}

topic_count() {
  awk '/^  Topic: / {count++} END {print count + 0}' "$LAST_INFO"
}

require_info() {
  grep -Fq "$1" "$LAST_INFO" || fail_case "missing info marker: $1"
}

require_record_log() {
  grep -Fq "$1" "$LAST_RECORD_LOG" || fail_case "missing record marker: $1"
}

require_topic() {
  local topic="$1"
  grep -Fxq "  Topic: $topic" "$LAST_INFO" || fail_case "missing topic: $topic"
}

forbid_topic() {
  local topic="$1"
  if grep -Fxq "  Topic: $topic" "$LAST_INFO"; then
    fail_case "unexpected topic: $topic"
  fi
}

require_topic_min_messages() {
  local topic="$1"
  local minimum="$2"
  local count
  count="$(topic_message_count "$topic")"
  [[ "$count" =~ ^[0-9]+$ && "$count" -ge "$minimum" ]] ||
    fail_case "topic $topic has $count messages, expected at least $minimum"
}

require_positive_topic() {
  require_topic_min_messages "$1" 5
}

require_common_bag_health() {
  local count
  count="$(message_count)"
  [[ "$count" =~ ^[0-9]+$ && "$count" -gt 0 ]] ||
    fail_case "bag has invalid message count '$count'"
  require_info "Closed cleanly:    yes"
  require_info "Dropped messages:  0"
  require_info "Duplicate copies:  0"
  require_info "Complete playback: yes"
}

require_empty_bag_health() {
  local count
  count="$(message_count)"
  [[ "$count" =~ ^[0-9]+$ && "$count" == 0 ]] ||
    fail_case "bag should be empty but has '$count' messages"
  require_info "Closed cleanly:    yes"
  require_info "Dropped messages:  0"
  require_info "Duplicate copies:  0"
  require_info "Complete playback: yes"
}

require_metadata_value() {
  local key="$1"
  local expected="$2"
  local metadata="$LAST_OUT/metadata.yaml"
  [[ -f "$metadata" ]] || fail_case "missing metadata: $metadata"
  grep -Eq "^[[:space:]]*$key:[[:space:]]*$expected[[:space:]]*$" "$metadata" ||
    fail_case "metadata $key is not $expected"
}

require_sqlite_pragma() {
  local pragma="$1"
  local expected="$2"
  local database
  database="$(find "$LAST_OUT" -maxdepth 1 -name '*.db3' -type f | head -1)"
  [[ -n "$database" ]] || fail_case "SQLite database is missing"
  local actual
  actual="$(python3 - "$database" "$pragma" <<'PY'
import sqlite3
import sys
db, pragma = sys.argv[1:]
with sqlite3.connect(db) as conn:
    row = conn.execute(f"PRAGMA {pragma}").fetchone()
print(row[0] if row else "")
PY
)"
  [[ "$actual" == "$expected" ]] ||
    fail_case "SQLite PRAGMA $pragma is '$actual', expected '$expected'"
}

run_case() {
  local label="$1"
  shift
  set +e
  (
    set -Eeuo pipefail
    CASE_CHILD=1
    trap cleanup EXIT INT TERM
    "$@"
  )
  local status=$?
  set -e
  if [[ "$status" != 0 ]]; then
    FAILED_CASES+=("$label")
    echo "record smoke: case failed: $label (status $status)" >&2
  fi
  return 0
}

run_transport_case() {
  local driver="$1"
  local mode="$2"
  local description="$3"
  local serialization="$4"
  CURRENT_CASE="transport_$driver"
  local topic="/ibmw_record_smoke/transport/$driver"
  echo "record smoke: transport $driver ($mode)"
  start_runtime "record_smoke_$driver.yaml" "record-smoke-$driver" \
    "$driver" "$topic" "$mode"
  start_record --registry "$topic" --polling-interval 20 \
    --storage mcap --serialization-format "$serialization"
  record_after_start "$topic"
  stop_record
  require_record_log "$topic"
  require_record_log "$description"
  stop_runtime
  read_info
  require_common_bag_health
  require_info "Storage id:        mcap"
  require_topic "$topic"
  require_positive_topic "$topic"
  if [[ "$serialization" == auto ]]; then
    require_info "Serialization: cdr"
  else
    require_info "Serialization: $serialization"
  fi
  [[ "$(topic_count)" == 1 ]] || fail_case "transport bag must contain exactly one topic"
  if [[ "$mode" == runtime_tap ]]; then
    grep -Fq "Record smoke received: kind='cdr', topic='$topic'" \
      "$CURRENT_RUNTIME_LOG" ||
      fail_case "$driver transport subscriber did not receive a message"
  fi
}

run_feature_case() {
  local name="$1"
  local cfg_name="$2"
  local runtime_id="$3"
  local hold_seconds="$4"
  local action="$5"
  shift 5
  CURRENT_CASE="$name"
  echo "record smoke: feature $name"
  start_runtime "$cfg_name" "$runtime_id"
  start_record "$@"
  sleep "$hold_seconds"
  case "$action" in
    none) ;;
    resume)
      kill -USR2 "$RECORD_PID"
      wait_for_log "$LAST_RECORD_LOG" "Recording resumed." "$RECORD_PID" ||
        fail_case "recorder did not resume"
      # Real recording window after the resume.
      sleep 0.8
      ;;
    snapshot)
      kill -USR1 "$RECORD_PID"
      wait_for_log "$LAST_RECORD_LOG" "Snapshot completed." "$RECORD_PID" ||
        fail_case "recorder did not complete the snapshot"
      ;;
    *) fail_case "unknown feature action: $action" ;;
  esac
  stop_record
  case "$action" in
    resume) require_record_log "Recording resumed." ;;
    snapshot) require_record_log "Snapshot completed." ;;
  esac
  stop_runtime
  read_info
}

run_late_case() {
  local name="$1"
  local no_discovery="$2"
  CURRENT_CASE="$name"
  echo "record smoke: feature $name"
  cleanup_runtime_files record-smoke-late
  local args=(/ibmw_record_smoke/features/late --polling-interval 50)
  [[ "$no_discovery" == true ]] && args+=(--no-discovery)
  # start_record returns after the initial discovery, so the topic is late.
  start_record "${args[@]}"
  start_runtime record_smoke_late.yaml record-smoke-late
  if [[ "$no_discovery" == true ]]; then
    # Negative window: the recorder must not pick up the late topic.
    sleep 1.2
  else
    record_after_start /ibmw_record_smoke/features/late
  fi
  stop_record
  stop_runtime
  read_info
}

start_services

run_case transport_local run_transport_case local runtime_tap \
  "runtime tap (runtime record-smoke-local)" auto
run_case transport_dds run_transport_case dds native_serialized \
  "dds subscriber (/ibmw_record_smoke/transport/dds)" cdr
if [[ "$ICEORYX_BUILT" == 1 ]]; then
  run_case transport_iceoryx run_transport_case iceoryx cached_serialized \
    "iceoryx cached serialized subscriber (runtime record-smoke-iceoryx)" cdr
fi
if [[ "$SHM_BUILT" == 1 ]]; then
  run_case transport_shm run_transport_case shm cached_serialized \
    "shm cached serialized subscriber (runtime record-smoke-shm)" cdr
fi
run_case transport_mqtt run_transport_case mqtt cached_serialized \
  "mqtt cached serialized subscriber (runtime record-smoke-mqtt)" cdr
run_case transport_zenoh run_transport_case zenoh native_serialized \
  "zenoh subscriber (/ibmw_record_smoke/transport/zenoh)" cdr
run_case transport_ros2 run_transport_case ros2 native_serialized \
  "ros2 subscriber (/ibmw_record_smoke/transport/ros2)" ros2
run_case transport_tcp run_transport_case tcp runtime_tap \
  "runtime tap (runtime record-smoke-tcp)" cdr
run_case transport_udp run_transport_case udp runtime_tap \
  "runtime tap (runtime record-smoke-udp)" cdr
run_case transport_http run_transport_case http runtime_tap \
  "runtime tap (runtime record-smoke-http)" cdr

run_feature_test() {
  local feature="$1"
  local data_topic=/ibmw_record_smoke/features/data
  local other_topic=/ibmw_record_smoke/features/other
  local hidden_topic=/_ibmw_record_smoke/features/hidden
  local leaf_topic=/ibmw_record_smoke/features/leaf

  case "$feature" in
    help)
      CURRENT_CASE=help
      echo "record smoke: feature help"
      "$IBMW_BIN" bag record --help >"$RUN_DIR/help.log"
      local help_options=(
        --all --regex --exclude --include-hidden-topics
        --include-unpublished-topics --ignore-leaf-topics --no-discovery
        --polling-interval --output --storage --serialization-format
        --max-bag-size --max-bag-duration --max-cache-size
        --compression-mode --compression-format --compression-queue-size
        --compression-threads --snapshot-mode --storage-preset-profile
        --storage-config-file --start-paused --use-sim-time --log-level)
      local option
      for option in "${help_options[@]}"; do
        grep -Fq -- "$option" "$RUN_DIR/help.log" ||
          fail_case "record help is missing $option"
      done
      ;;
    explicit_cdr)
      run_feature_case explicit_cdr record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" \
        --storage=mcap --serialization-format=cdr
      require_common_bag_health
      require_info "Storage id:        mcap"
      require_info "Serialization: cdr"
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      [[ "$(topic_count)" == 1 ]] ||
        fail_case "explicit selection contains extra topics"
      ;;
    all_default)
      run_feature_case all_default record_smoke_features.yaml \
        record-smoke-features 1.0 none --all
      require_common_bag_health
      require_topic "$data_topic"
      require_topic "$other_topic"
      require_positive_topic "$data_topic"
      require_positive_topic "$other_topic"
      forbid_topic "$hidden_topic"
      forbid_topic "$leaf_topic"
      [[ "$(topic_count)" == 2 ]] ||
        fail_case "--all did not select exactly two public topics"
      ;;
    all_including_special)
      run_feature_case all_including_special record_smoke_features.yaml \
        record-smoke-features 1.0 none \
        --all --include-hidden-topics --include-unpublished-topics
      require_common_bag_health
      require_topic "$data_topic"
      require_topic "$other_topic"
      require_topic "$hidden_topic"
      require_topic "$leaf_topic"
      require_positive_topic "$data_topic"
      require_positive_topic "$other_topic"
      require_positive_topic "$hidden_topic"
      [[ "$(topic_message_count "$leaf_topic")" == 0 ]] ||
        fail_case "unpublished leaf topic unexpectedly has messages"
      [[ "$(topic_count)" == 4 ]] ||
        fail_case "special-topic selection did not contain exactly four topics"
      ;;
    regex_exclude)
      run_feature_case regex_exclude record_smoke_features.yaml \
        record-smoke-features 1.0 none \
        --regex '^/ibmw_record_smoke/features/(data|other)$' --exclude 'other$'
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      forbid_topic "$other_topic"
      [[ "$(topic_count)" == 1 ]] ||
        fail_case "regex/exclude selection contains extra topics"
      ;;
    ignore_leaf)
      run_feature_case ignore_leaf record_smoke_features.yaml \
        record-smoke-features 1.0 none \
        --all --include-unpublished-topics --ignore-leaf-topics
      require_common_bag_health
      require_topic "$data_topic"
      require_topic "$other_topic"
      require_positive_topic "$data_topic"
      require_positive_topic "$other_topic"
      forbid_topic "$leaf_topic"
      ;;
    json_serialization)
      run_feature_case json_serialization record_smoke_json.yaml \
        record-smoke-json 1.0 none \
        /ibmw_record_smoke/features/json --serialization-format json
      require_common_bag_health
      require_topic /ibmw_record_smoke/features/json
      require_positive_topic /ibmw_record_smoke/features/json
      require_info "Serialization: json"
      ;;
    transcode_cdr_to_json)
      run_feature_case transcode_cdr_to_json record_smoke_transcode.yaml \
        record-smoke-transcode 1.0 none \
        /ibmw_record_smoke/features/transcode --serialization-format json
      require_common_bag_health
      require_topic /ibmw_record_smoke/features/transcode
      require_positive_topic /ibmw_record_smoke/features/transcode
      require_info "Serialization: json"
      require_info "encoding=jsonschema"
      ;;
    sqlite_preset)
      run_feature_case sqlite_preset record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" --storage sqlite3 \
        --storage-preset-profile resilient
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_info "Storage id:        sqlite3"
      require_sqlite_pragma journal_mode wal
      require_sqlite_pragma synchronous 2
      ;;
    sqlite_config)
      run_feature_case sqlite_config record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" --storage sqlite3 \
        --storage-config-file "$CFG_DIR/record_smoke_sqlite_storage.yaml"
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_info "Storage id:        sqlite3"
      require_sqlite_pragma journal_mode wal
      ;;
    paused_without_resume)
      run_feature_case paused_without_resume record_smoke_features.yaml \
        record-smoke-features 0.7 none "$data_topic" --start-paused
      require_empty_bag_health
      require_record_log "Recording starts paused"
      ;;
    start_paused)
      run_feature_case start_paused record_smoke_features.yaml \
        record-smoke-features 0.7 resume "$data_topic" --start-paused
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_record_log "Recording starts paused"
      require_record_log "Recording resumed."
      ;;
    snapshot_without_signal)
      run_feature_case snapshot_without_signal record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" --snapshot-mode
      require_empty_bag_health
      require_record_log "Snapshot mode:"
      ;;
    snapshot)
      run_feature_case snapshot record_smoke_features.yaml \
        record-smoke-features 1.0 snapshot "$data_topic" --snapshot-mode
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_record_log "Snapshot completed."
      ;;
    snapshot_cache_limit)
      run_feature_case snapshot_cache_limit record_smoke_features.yaml \
        record-smoke-features 1.0 snapshot "$data_topic" --snapshot-mode \
        --max-cache-size 128
      local cache_messages cache_dropped
      cache_messages="$(message_count)"
      cache_dropped="$(info_value 'Dropped messages')"
      [[ "$cache_messages" =~ ^[0-9]+$ && "$cache_messages" -ge 1 ]] ||
        fail_case "snapshot cache produced no messages"
      [[ "$cache_messages" -le 4 ]] ||
        fail_case "max-cache-size did not bound snapshot (messages=$cache_messages)"
      [[ "$cache_dropped" =~ ^[1-9][0-9]*$ ]] ||
        fail_case "max-cache-size did not drop buffered messages"
      require_info "Closed cleanly:    yes"
      require_info "Duplicate copies:  0"
      require_info "Complete playback: yes"
      require_record_log "Snapshot completed."
      ;;
    split_size)
      run_feature_case split_size record_smoke_features.yaml \
        record-smoke-features 1.6 none "$data_topic" --max-bag-size 1200
      require_common_bag_health
      [[ "$(file_count)" -gt 1 ]] ||
        fail_case "size split did not create multiple files"
      require_info "split_reason=size"
      ;;
    split_duration)
      run_feature_case split_duration record_smoke_features.yaml \
        record-smoke-features 1.2 none "$data_topic" --max-bag-duration=0.2
      require_common_bag_health
      [[ "$(file_count)" -gt 1 ]] ||
        fail_case "duration split did not create multiple files"
      require_info "split_reason=duration"
      ;;
    compression_message)
      run_feature_case compression_message record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" \
        --compression-mode message --compression-format zstd \
        --compression-queue-size 2 --compression-threads=0 \
        --storage-preset-profile small \
        --storage-config-file "$CFG_DIR/record_smoke_mcap_storage.yaml"
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_info "Compression:       message/zstd"
      require_metadata_value compression_queue_size 2
      local compression_threads
      compression_threads="$(awk -F: '$1 ~ /compression_threads/ {gsub(/[[:space:]]/, "", $2); print $2; exit}' "$LAST_OUT/metadata.yaml")"
      [[ "$compression_threads" =~ ^[1-9][0-9]*$ ]] ||
        fail_case "compression_threads metadata is '$compression_threads'"
      ;;
    compression_file)
      run_feature_case compression_file record_smoke_features.yaml \
        record-smoke-features 1.0 none "$data_topic" \
        --compression-mode=file --compression-format=lz4 \
        --compression-queue-size=2 --compression-threads 2
      require_common_bag_health
      require_topic "$data_topic"
      require_positive_topic "$data_topic"
      require_info "Compression:       file/lz4"
      require_metadata_value compression_queue_size 2
      require_metadata_value compression_threads 2
      find "$LAST_OUT" -maxdepth 1 -name '*.mcap.lz4' -type f | grep -q . ||
        fail_case "file compression did not create an .mcap.lz4 file"
      ;;
    sim_time)
      run_feature_case sim_time record_smoke_sim_time.yaml \
        record-smoke-sim-time 1.2 none /clock \
        /ibmw_record_smoke/features/sim_data --use-sim-time \
        --serialization-format ros2
      require_common_bag_health
      require_topic /clock
      require_topic /ibmw_record_smoke/features/sim_data
      require_positive_topic /clock
      require_positive_topic /ibmw_record_smoke/features/sim_data
      require_info "Start:             100123456789 ns"
      grep -Fq "Record smoke clock publisher active" "$CURRENT_RUNTIME_LOG" ||
        fail_case "real Clock publisher did not activate"
      ;;
    dynamic_discovery)
      run_late_case dynamic_discovery false
      require_common_bag_health
      require_topic /ibmw_record_smoke/features/late
      require_positive_topic /ibmw_record_smoke/features/late
      ;;
    no_discovery)
      run_late_case no_discovery true
      require_empty_bag_health
      forbid_topic /ibmw_record_smoke/features/late
      ;;
    polling_interval)
      CURRENT_CASE=polling_interval
      echo "record smoke: feature polling_interval"
      cleanup_runtime_files record-smoke-late
      start_record /ibmw_record_smoke/features/late --polling-interval 1500
      start_runtime record_smoke_late.yaml record-smoke-late
      # Negative window, shorter than the 1.5 s polling interval.
      sleep 0.6
      stop_record
      stop_runtime
      read_info
      require_empty_bag_health
      forbid_topic /ibmw_record_smoke/features/late
      ;;
    log_level)
      local dds_topic=/ibmw_record_smoke/transport/dds_native_dds_ibmw__protocols__common__Header
      local recorder_config="$RUN_DIR/dds-recorder.yaml"
      local transport_library="$BUILD_DIR/libibmw_dds_transport.so"
      local type_library="$BUILD_DIR/libcommon_dds_ts.so"
      [[ -f "$transport_library" ]] || transport_library="$BUILD_DIR/lib/libibmw_dds_transport.so"
      [[ -f "$type_library" ]] || type_library="$BUILD_DIR/lib/libcommon_dds_ts.so"
      cat >"$recorder_config" <<EOF
ibmw:
  runtime_graph:
    type_support_pkgs: ["$type_library"]
  extensions:
    items:
      - name: dds_extension
        path: "$transport_library"
        options: {domain_id: 87}
  messaging:
    drivers: [{type: dds}]
EOF
      run_feature_case log_level_info record_smoke_dds.yaml \
        record-smoke-dds 1.0 none "$dds_topic" --dds --config-file "$recorder_config" --log-level info
      require_common_bag_health
      require_topic "$dds_topic"
      require_positive_topic "$dds_topic"
      require_record_log "DDS transport driver initialized."

      run_feature_case log_level_error record_smoke_dds.yaml \
        record-smoke-dds 1.0 none "$dds_topic" --dds --config-file "$recorder_config" --log-level error
      require_common_bag_health
      require_topic "$dds_topic"
      require_positive_topic "$dds_topic"
      if grep -Fq "DDS transport driver initialized." "$LAST_RECORD_LOG"; then
        fail_case "--log-level error did not suppress recorder Info output"
      fi
      ;;
    *)
      fail_case "unknown feature test: $feature"
      ;;
  esac
}

feature_tests=(
  help explicit_cdr all_default all_including_special regex_exclude
  ignore_leaf json_serialization transcode_cdr_to_json sqlite_preset
  sqlite_config paused_without_resume start_paused snapshot_without_signal
  snapshot snapshot_cache_limit split_size split_duration compression_message
  compression_file sim_time dynamic_discovery no_discovery polling_interval
  log_level)
for feature_test in "${feature_tests[@]}"; do
  run_case "feature_$feature_test" run_feature_test "$feature_test"
done

if [[ "${#FAILED_CASES[@]}" -gt 0 ]]; then
  echo "FAIL: record smoke cases: ${FAILED_CASES[*]}" >&2
  echo "record smoke artifacts: $RUN_DIR" >&2
  exit 1
fi

echo "PASS: record smoke transport and CLI matrix"
echo "record smoke artifacts: $RUN_DIR"
