#!/bin/sh
set -u

ROOT=${IBMW_TOPOLOGY_ROOT:-/data/local/tmp/ibmw-ohos-topology}
LANE=${1:-}
RUN_ID=${2:-}
COUNT=${3:-3000}
WARMUP=50
HZ=100
RUN="$ROOT/runs/$RUN_ID"
source_pid= relay_pid= sink_a_pid= sink_b_pid=
source_ready="$RUN/source.ready"
relay_ready="$RUN/relay.ready"
start_gate="$RUN/start.gate"

active() { kill -0 "$1" 2>/dev/null && [ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)" != Z ]; }
stop() { [ -n "$1" ] && kill -TERM "$1" 2>/dev/null || true; }
cleanup() { stop "$source_pid"; stop "$relay_pid"; stop "$sink_a_pid"; stop "$sink_b_pid"; wait "$source_pid" 2>/dev/null || true; wait "$relay_pid" 2>/dev/null || true; wait "$sink_a_pid" 2>/dev/null || true; wait "$sink_b_pid" 2>/dev/null || true; }
trap cleanup EXIT HUP INT TERM

case "$LANE" in A|B) ;; *) exit 2 ;; esac
case "$RUN_ID" in ''|*[!A-Za-z0-9._-]*) exit 2 ;; esac
case "$COUNT" in ''|*[!0-9]*|0) exit 2 ;; esac
[ ! -e "$RUN" ] || exit 2
mkdir -p "$RUN" || exit 1
export MW_PERF_RUN_ID="$RUN_ID"
export MW_PERF_ARTIFACT_SCHEMA_VERSION=mw_perf.topology.v1

if [ "$LANE" = A ]; then
  export LD_LIBRARY_PATH="$ROOT/lib-a"
  export AMENT_PREFIX_PATH="$ROOT"
  export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
  export ROS_DOMAIN_ID=45
  export ROS_LOG_DIR="$RUN"
  endpoint="$ROOT/bin/ibmw_topology_ros2_endpoint"
  config() { :; }
else
  cd "$ROOT/lib-b" || exit 1
  export LD_LIBRARY_PATH="$ROOT/lib-b"
  endpoint="$ROOT/bin/ibmw_topology_dds_endpoint"
  config() { printf '%s' "--config $ROOT/cfg/$1.yaml"; }
fi

MW_PERF_READY_FILE="$source_ready" MW_PERF_START_GATE="$start_gate" \
"$endpoint" --role source $(config publisher) --hz "$HZ" --warmup-messages "$WARMUP" \
  --message-count "$COUNT" --status "$RUN/source.status.json" \
  >"$RUN/source.log" 2>&1 &
source_pid=$!
active "$source_pid" || exit 1
producer_ticks=$(awk '{print $22}' "/proc/$source_pid/stat") || exit 1
export MW_PERF_PRODUCER_PID="$source_pid"
export MW_PERF_PRODUCER_START_TIME_TICKS="$producer_ticks"

MW_PERF_READY_FILE="$relay_ready" \
"$endpoint" --role relay $(config relay) --hz "$HZ" --warmup-messages "$WARMUP" \
  --message-count "$COUNT" --status "$RUN/relay.status.json" \
  >"$RUN/relay.log" 2>&1 &
relay_pid=$!
"$endpoint" --role sink --sink-id A $(config sink_a) --warmup-messages "$WARMUP" \
  --message-count "$COUNT" --status "$RUN/A.status.json" --csv "$RUN/A.csv" \
  --jsonl "$RUN/A.jsonl" >"$RUN/sinkA.log" 2>&1 &
sink_a_pid=$!
"$endpoint" --role sink --sink-id B $(config sink_b) --warmup-messages "$WARMUP" \
  --message-count "$COUNT" --status "$RUN/B.status.json" --csv "$RUN/B.csv" \
  --jsonl "$RUN/B.jsonl" >"$RUN/sinkB.log" 2>&1 &
sink_b_pid=$!
deadline=30
while [ "$deadline" -gt 0 ] && { [ ! -e "$source_ready" ] || [ ! -e "$relay_ready" ]; }; do
  active "$source_pid" || { printf 'source exited before readiness\n' >&2; exit 1; }
  active "$relay_pid" || { printf 'relay exited before readiness\n' >&2; exit 1; }
  active "$sink_a_pid" || { printf 'sink A exited before readiness\n' >&2; exit 1; }
  active "$sink_b_pid" || { printf 'sink B exited before readiness\n' >&2; exit 1; }
  sleep 1
  deadline=$((deadline - 1))
done
[ -e "$source_ready" ] && [ -e "$relay_ready" ] || { printf 'readiness timeout: source=%s relay=%s\n' "$( [ -e "$source_ready" ] && printf true || printf false )" "$( [ -e "$relay_ready" ] && printf true || printf false )" >&2; exit 1; }
active "$source_pid" && active "$relay_pid" && active "$sink_a_pid" && active "$sink_b_pid" || exit 1
gate_tmp="$RUN/start.gate.tmp"
: >"$gate_tmp" || exit 1
mv -f "$gate_tmp" "$start_gate" || exit 1

printf '%s %s %s %s\n' "$source_pid" "$relay_pid" "$sink_a_pid" "$sink_b_pid" >"$RUN/pids.txt"
sleep 5
cp /proc/uptime "$RUN/uptime-start.txt"
for pair in source:$source_pid relay:$relay_pid sinkA:$sink_a_pid sinkB:$sink_b_pid; do
  role=${pair%%:*}; pid=${pair#*:}; active "$pid" || exit 1
  cp "/proc/$pid/stat" "$RUN/$role-stat-start.txt" || exit 1
done
sleep 7
for pair in source:$source_pid relay:$relay_pid sinkA:$sink_a_pid sinkB:$sink_b_pid; do
  role=${pair%%:*}; pid=${pair#*:}; active "$pid" || exit 1
  cp "/proc/$pid/status" "$RUN/$role-status-mid.txt" 2>/dev/null || true
  cp "/proc/$pid/smaps_rollup" "$RUN/$role-smaps-mid.txt" 2>/dev/null || true
done
top -b -n 1 -p "$source_pid,$relay_pid,$sink_a_pid,$sink_b_pid" -O %CPU,TIME+,PID,ARGS >"$RUN/top-mid.txt" 2>&1 || true
sleep 8
cp /proc/uptime "$RUN/uptime-end.txt"
for pair in source:$source_pid relay:$relay_pid sinkA:$sink_a_pid sinkB:$sink_b_pid; do
  role=${pair%%:*}; pid=${pair#*:}; active "$pid" || exit 1
  cp "/proc/$pid/stat" "$RUN/$role-stat-end.txt" || exit 1
done
top -b -n 1 -p "$source_pid,$relay_pid,$sink_a_pid,$sink_b_pid" -O %CPU,TIME+,PID,ARGS >"$RUN/top-end.txt" 2>&1 || true

deadline=45
while [ "$deadline" -gt 0 ] && { active "$source_pid" || active "$sink_a_pid" || active "$sink_b_pid"; }; do
  sleep 1; deadline=$((deadline - 1))
done
active "$source_pid" && exit 1
active "$sink_a_pid" && exit 1
active "$sink_b_pid" && exit 1
wait "$source_pid"; source_rc=$?; source_pid=
wait "$sink_a_pid"; sink_a_rc=$?; sink_a_pid=
wait "$sink_b_pid"; sink_b_rc=$?; sink_b_pid=
stop "$relay_pid"; wait "$relay_pid" 2>/dev/null; relay_pid=
printf 'source=%s sink_a=%s sink_b=%s\n' "$source_rc" "$sink_a_rc" "$sink_b_rc" >"$RUN/exit-codes.txt"

expected=$((COUNT + 1))
[ "$source_rc" -eq 0 ] && [ "$sink_a_rc" -eq 0 ] && [ "$sink_b_rc" -eq 0 ] || exit 1
[ "$(wc -l <"$RUN/A.jsonl")" -eq "$expected" ] || exit 1
[ "$(wc -l <"$RUN/B.jsonl")" -eq "$expected" ] || exit 1
grep -q '"valid":true' "$RUN/A.status.json" || exit 1
grep -q '"valid":true' "$RUN/B.status.json" || exit 1
printf 'lane=%s run_id=%s records_per_sink=%s valid=true\n' "$LANE" "$RUN_ID" "$COUNT"
