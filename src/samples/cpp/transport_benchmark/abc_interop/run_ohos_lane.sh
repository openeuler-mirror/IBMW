#!/bin/sh
set -u

ROOT="${IBMW_ABC_ROOT:-/data/local/tmp/ibmw-ohos-abc}"
WARMUP=50
HZ=100
CPU_CLK_TCK=100
CPU_STARTUP_SETTLE_SECONDS=5
SCHEMA=mw_perf.latency.v1

usage() {
  printf 'Usage: %s A|B RUN_ID [COUNT]\n' "$0" >&2
  exit 2
}

[ "$#" -ge 2 ] && [ "$#" -le 3 ] || usage
LANE=$1
RUN_ID=$2
COUNT=${3:-3000}
case "$LANE" in A|B) ;; *) usage ;; esac
case "$RUN_ID" in ''|*[!A-Za-z0-9._-]*) usage ;; esac
case "$COUNT" in ''|*[!0-9]*|0) usage ;; esac
case "$ROOT" in /*) ;; *) printf 'IBMW_ABC_ROOT must be absolute\n' >&2; exit 2 ;; esac
case "$ROOT" in *[!A-Za-z0-9_./-]*) printf 'Invalid IBMW_ABC_ROOT\n' >&2; exit 2 ;; esac

RUN="$ROOT/runs/$RUN_ID"
LOCK="$ROOT/.abc-lane.lock"
sub_pid=
pub_pid=
lock_held=false
cpu_end_captured=false

pid_active() {
  pid=$1
  kill -0 "$pid" 2>/dev/null || return 1
  if [ -r "/proc/$pid/stat" ]; then
    state=
    read -r _ _ state _ <"/proc/$pid/stat" || return 1
    [ "$state" != Z ] || return 1
  fi
  return 0
}

stop_pid() {
  pid=$1
  signal=$2
  [ -n "$pid" ] || return 0
  pid_active "$pid" || return 0
  kill "-$signal" "$pid" 2>/dev/null || true
  tries=0
  while pid_active "$pid" && [ "$tries" -lt 10 ]; do
    sleep 1
    tries=$((tries + 1))
  done
  if pid_active "$pid"; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
}

capture_cpu_start() {
  pid_active "$pub_pid" && pid_active "$sub_pid" || return 1
  cp /proc/uptime "$RUN/cpu-uptime-start.txt" 2>/dev/null || return 1
  cp "/proc/$pub_pid/stat" "$RUN/pub-proc-stat-start.txt" 2>/dev/null || return 1
  cp "/proc/$sub_pid/stat" "$RUN/sub-proc-stat-start.txt" 2>/dev/null || return 1
  pid_active "$pub_pid" && pid_active "$sub_pid"
}

capture_cpu_end() {
  [ "$cpu_end_captured" = false ] || return 0
  pid_active "$pub_pid" && pid_active "$sub_pid" || return 1
  cp /proc/uptime "$RUN/.cpu-uptime-end.txt" 2>/dev/null || return 1
  cp "/proc/$pub_pid/stat" "$RUN/.pub-proc-stat-end.txt" 2>/dev/null || return 1
  cp "/proc/$sub_pid/stat" "$RUN/.sub-proc-stat-end.txt" 2>/dev/null || return 1
  if ! pid_active "$pub_pid" || ! pid_active "$sub_pid"; then
    rm -f "$RUN/.cpu-uptime-end.txt" "$RUN/.pub-proc-stat-end.txt" \
      "$RUN/.sub-proc-stat-end.txt"
    return 1
  fi
  mv "$RUN/.cpu-uptime-end.txt" "$RUN/cpu-uptime-end.txt" || return 1
  mv "$RUN/.pub-proc-stat-end.txt" "$RUN/pub-proc-stat-end.txt" || return 1
  mv "$RUN/.sub-proc-stat-end.txt" "$RUN/sub-proc-stat-end.txt" || return 1
  cpu_end_captured=true
}

top_snapshot_has_pid() {
  snapshot=$1
  pid=$2
  [ -s "$snapshot" ] && awk -v expected="$pid" '
    { for (i = 1; i <= NF; i++) if ($i == expected) found=1 }
    END { exit(found ? 0 : 1) }
  ' "$snapshot"
}

make_cpu_summary() {
  : >"$RUN/cpu-summary.json"
  awk -v clk="$CPU_CLK_TCK" '
    FILENAME == ARGV[1] { start = $1; next }
    FILENAME == ARGV[2] { finish = $1; next }
    FILENAME == ARGV[3] { pub_u = $14; pub_s = $15; next }
    FILENAME == ARGV[4] { sub_u = $14; sub_s = $15; next }
    FILENAME == ARGV[5] { pub_ue = $14; pub_se = $15; next }
    FILENAME == ARGV[6] { sub_ue = $14; sub_se = $15; next }
    END {
      elapsed = finish - start
      pub = ((pub_ue - pub_u) + (pub_se - pub_s)) / clk
      sub_cpu = ((sub_ue - sub_u) + (sub_se - sub_s)) / clk
      if (start !~ /^[0-9][0-9]*([.][0-9][0-9]*)?$/ || finish !~ /^[0-9][0-9]*([.][0-9][0-9]*)?$/ || elapsed <= 0 ||
          pub_u !~ /^[0-9][0-9]*$/ || pub_s !~ /^[0-9][0-9]*$/ || sub_u !~ /^[0-9][0-9]*$/ || sub_s !~ /^[0-9][0-9]*$/ ||
          pub_ue !~ /^[0-9][0-9]*$/ || pub_se !~ /^[0-9][0-9]*$/ || sub_ue !~ /^[0-9][0-9]*$/ || sub_se !~ /^[0-9][0-9]*$/ || pub < 0 || sub_cpu < 0) exit 1
      printf "{\"pub_cpu_pct\": %.6f, \"sub_cpu_pct\": %.6f, \"pair_cpu_pct\": %.6f, \"elapsed_seconds\": %.6f, \"clk_tck\": %d, \"source\": \"proc_stat_fields_14_15_and_proc_uptime\"}\n", pub / elapsed * 100, sub_cpu / elapsed * 100, (pub + sub_cpu) / elapsed * 100, elapsed, clk
    }
  ' "$RUN/cpu-uptime-start.txt" "$RUN/cpu-uptime-end.txt" "$RUN/pub-proc-stat-start.txt" "$RUN/sub-proc-stat-start.txt" "$RUN/pub-proc-stat-end.txt" "$RUN/sub-proc-stat-end.txt" >"$RUN/cpu-summary.json"
}

cpu_summary_numeric() {
  [ -s "$RUN/cpu-summary.json" ] && awk '
    /"pub_cpu_pct":[[:space:]]*[0-9][0-9]*[.][0-9][0-9]*/ && /"sub_cpu_pct":[[:space:]]*[0-9][0-9]*[.][0-9][0-9]*/ && /"pair_cpu_pct":[[:space:]]*[0-9][0-9]*[.][0-9][0-9]*/ && /"elapsed_seconds":[[:space:]]*[0-9][0-9]*[.][0-9][0-9]*/ && /"clk_tck":[[:space:]]*100,/ { found=1 }
    END { exit(found ? 0 : 1) }
  ' "$RUN/cpu-summary.json"
}

cleanup() {
  stop_pid "$pub_pid" TERM
  if [ "$LANE" = A ]; then
    stop_pid "$sub_pid" INT
  else
    stop_pid "$sub_pid" TERM
  fi
  if [ "$lock_held" = true ]; then
    rmdir "$LOCK" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$ROOT" || exit 1
if ! mkdir "$LOCK" 2>/dev/null; then
  printf 'Another OHOS A/B lane is active: %s\n' "$LOCK" >&2
  exit 1
fi
lock_held=true
if [ -e "$RUN" ]; then
  printf 'Run directory already exists: %s\n' "$RUN" >&2
  exit 1
fi
mkdir -p "$RUN/logs" || exit 1

export MW_PERF_RUN_ID="$RUN_ID"
export MW_PERF_ARTIFACT_SCHEMA_VERSION="$SCHEMA"

if [ "$LANE" = A ]; then
  export LD_LIBRARY_PATH="$ROOT/lib-a"
  export AMENT_PREFIX_PATH="$ROOT"
  export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
  export ROS_DOMAIN_ID=45
  export ROS_LOG_DIR="$RUN/logs"
  "$ROOT/bin/ibmw_abc_ros2_endpoint" \
    --role pub --hz "$HZ" --warmup-messages "$WARMUP" \
    --message-count "$COUNT" --status "$RUN/pub-status.json" \
    >"$RUN/pub.log" 2>&1 &
  pub_pid=$!
else
  cd "$ROOT/lib-b" || exit 1
  export LD_LIBRARY_PATH="$ROOT/lib-b"
  "$ROOT/bin/ibmw_abc_dds_endpoint" \
    --role pub --config "$ROOT/cfg/publisher.yaml" --hz "$HZ" \
    --warmup-messages "$WARMUP" --message-count "$COUNT" \
    --status "$RUN/pub-status.json" >"$RUN/pub.log" 2>&1 &
  pub_pid=$!
fi

if ! pid_active "$pub_pid"; then
  printf 'Publisher exited before identity handoff: %s\n' "$pub_pid" >&2
  exit 1
fi
producer_start_ticks=$(awk '{print $22}' "/proc/$pub_pid/stat" 2>/dev/null) || {
  printf 'Failed to read publisher start-time ticks: %s\n' "$pub_pid" >&2
  exit 1
}
case "$producer_start_ticks" in
  ''|*[!0-9]*|0)
    printf 'Invalid publisher start-time ticks: %s\n' "$producer_start_ticks" >&2
    exit 1
    ;;
esac
export MW_PERF_PRODUCER_PID="$pub_pid"
export MW_PERF_PRODUCER_START_TIME_TICKS="$producer_start_ticks"

if [ "$LANE" = A ]; then
  "$ROOT/bin/ibmw_abc_ros2_endpoint" \
    --role sub --warmup-messages "$WARMUP" --message-count "$COUNT" \
    --csv "$RUN/latency.csv" --status "$RUN/sub-status.json" \
    --jsonl "$RUN/latency.jsonl" >"$RUN/sub.log" 2>&1 &
else
  "$ROOT/bin/ibmw_abc_dds_endpoint" \
    --role sub --config "$ROOT/cfg/subscriber.yaml" \
    --warmup-messages "$WARMUP" --message-count "$COUNT" \
    --csv "$RUN/latency.csv" --status "$RUN/sub-status.json" \
    --jsonl "$RUN/latency.jsonl" >"$RUN/sub.log" 2>&1 &
fi
sub_pid=$!
validation_sub_pid=$sub_pid
if ! pid_active "$pub_pid" || ! pid_active "$sub_pid"; then
  printf 'Both processes must remain active during CPU startup settle\n' >&2
  exit 1
fi
sleep "$CPU_STARTUP_SETTLE_SECONDS"
if ! capture_cpu_start; then
  printf 'Failed to capture CPU start boundary with both processes active\n' >&2
  exit 1
fi

# Leave a five-second margin for publisher-first discovery/startup after the
# startup settle and CPU start boundary. Both processes must still be active at
# that boundary; an early exit makes the CPU evidence invalid.
measurement_seconds=$(((WARMUP + COUNT) / HZ - CPU_STARTUP_SETTLE_SECONDS - 5))
[ "$measurement_seconds" -gt 0 ] || measurement_seconds=1
midpoint_seconds=$(((measurement_seconds + 1) / 2))
[ "$midpoint_seconds" -gt 0 ] || midpoint_seconds=1
sleep "$midpoint_seconds"
for role_pid in "sub:$sub_pid" "pub:$pub_pid"; do
  role=${role_pid%%:*}
  pid=${role_pid#*:}
  if pid_active "$pid"; then
    cp "/proc/$pid/status" "$RUN/$role-proc-status-mid.txt" 2>/dev/null || true
    cp "/proc/$pid/smaps_rollup" "$RUN/$role-smaps-rollup-mid.txt" 2>/dev/null || true
  fi
done
top -b -n 1 -p "$pub_pid,$sub_pid" -O %CPU,TIME+,PID,ARGS >"$RUN/top-mid.txt" 2>&1 || true

deadline=$((measurement_seconds + 45))
elapsed=$midpoint_seconds
while [ "$elapsed" -lt "$measurement_seconds" ] && \
      [ "$elapsed" -lt "$deadline" ] && \
      pid_active "$pub_pid" && pid_active "$sub_pid"; do
  sleep 1
  elapsed=$((elapsed + 1))
done
if [ "$elapsed" -ge "$measurement_seconds" ] && \
   pid_active "$pub_pid" && pid_active "$sub_pid"; then
  capture_cpu_end || true
  top -b -n 1 -p "$pub_pid,$sub_pid" -O %CPU,TIME+,PID,ARGS >"$RUN/top-end.txt" 2>&1 || true
fi

while pid_active "$pub_pid" && [ "$elapsed" -lt "$deadline" ]; do
  sleep 1
  elapsed=$((elapsed + 1))
done
if pid_active "$pub_pid"; then
  stop_pid "$pub_pid" TERM
  wait "$pub_pid" 2>/dev/null || true
  pub_rc=124
else
  wait "$pub_pid"
  pub_rc=$?
fi
pub_pid=

sleep 1
if [ "$LANE" = A ]; then
  stop_pid "$sub_pid" INT
else
  stop_pid "$sub_pid" TERM
fi
wait "$sub_pid" 2>/dev/null
sub_rc=$?
sub_pid=

if [ "$cpu_end_captured" = true ]; then
  make_cpu_summary || true
else
  : >"$RUN/cpu-summary.json"
fi

top_snapshot_valid=false
if top_snapshot_has_pid "$RUN/top-mid.txt" "$MW_PERF_PRODUCER_PID" && \
   top_snapshot_has_pid "$RUN/top-mid.txt" "$validation_sub_pid" && \
   top_snapshot_has_pid "$RUN/top-end.txt" "$MW_PERF_PRODUCER_PID" && \
   top_snapshot_has_pid "$RUN/top-end.txt" "$validation_sub_pid"; then
  top_snapshot_valid=true
fi

expected_lines=$((COUNT + 1))
actual_lines=0
if [ -f "$RUN/latency.jsonl" ]; then
  actual_lines=$(wc -l <"$RUN/latency.jsonl" 2>/dev/null) || actual_lines=0
fi
valid=false
if [ "$pub_rc" -eq 0 ] && [ "$sub_rc" -eq 0 ] && \
   [ -s "$RUN/pub-status.json" ] && [ -s "$RUN/sub-status.json" ] && \
   [ "$actual_lines" -eq "$expected_lines" ] && \
   [ "$cpu_end_captured" = true ] && [ "$top_snapshot_valid" = true ] && \
   grep -q "\"attempted_measured\": $COUNT[[:space:]]*$" "$RUN/pub-status.json" 2>/dev/null && \
   grep -q "\"attempted_measured\": $COUNT[[:space:]]*,[[:space:]]*$" "$RUN/sub-status.json" 2>/dev/null && \
   grep -q '"valid": true' "$RUN/sub-status.json" 2>/dev/null && \
   tail -n 1 "$RUN/latency.jsonl" 2>/dev/null | grep -q '"complete":true' && \
   cpu_summary_numeric && \
   "$ROOT/validate_ohos_artifact.sh" "$RUN/latency.jsonl" "$COUNT" "$RUN_ID" \
     "$MW_PERF_PRODUCER_PID" "$MW_PERF_PRODUCER_START_TIME_TICKS"; then
  valid=true
fi
records=$((actual_lines > 0 ? actual_lines - 1 : 0))
printf 'lane=%s run_id=%s pub_rc=%s sub_rc=%s records=%s valid=%s\n' \
  "$LANE" "$RUN_ID" "$pub_rc" "$sub_rc" "$records" "$valid"
[ "$valid" = true ]
