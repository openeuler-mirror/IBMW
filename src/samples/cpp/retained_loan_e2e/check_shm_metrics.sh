#!/bin/sh
# Non-managed check of shm_retained_loan_e2e (CI / ctest): runs the direct and
# the retained workload to completion (no MW_PERF_MANAGED, so they exit by
# themselves and write the final Reporter V1 snapshot), checks the PASS markers
# and runs assert_shm_metrics.py on each metrics.jsonl. Each workload gets a
# private discovery root under /tmp (never /dev/shm), removed on exit. Only the
# processes started here are signalled, with bounded timeouts.
set -u

if [ "$#" -ne 2 ]; then
  echo "usage: $0 BUILD_DIR WORK_DIR" >&2
  exit 2
fi

BUILD_DIR=$1
WORK_DIR=$2
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-python3}
E2E="$BUILD_DIR/shm_retained_loan_e2e"
if [ ! -x "$E2E" ]; then
  echo "shm_retained_loan_e2e is not executable: $E2E" >&2
  exit 2
fi
mkdir -p "$WORK_DIR"

SHM_ROOT=""
children=""
cleanup() {
  if [ -n "$children" ]; then
    for pid in $children; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in $children; do wait "$pid" 2>/dev/null || true; done
    children=""
  fi
  if [ -n "$SHM_ROOT" ]; then
    rm -rf "$SHM_ROOT"
    SHM_ROOT=""
  fi
}
# HUP too: dash does not run the EXIT trap when a signal kills the shell.
# (A SIGKILL of this shell still leaves the root and the children behind.)
trap 'trap "" TERM INT HUP; cleanup; exit 1' TERM INT HUP
trap cleanup EXIT

fail() {
  echo "SHM_RETAINED_E2E_CHECK_FAIL mode=$1 reason=$2" >&2
  for log in "$WORK_DIR/$1"/*.log; do
    [ -f "$log" ] || continue
    echo "---- $log" >&2
    tail -n 20 "$log" >&2
  done
  exit 1
}

# Fresh private root and domain for one workload. The service directory
# <root>/ibmw_shm.<uid>.<domain> must fit the library's 63-byte limit.
new_root() {
  cleanup
  SHM_ROOT=$(mktemp -d /tmp/ibR.XXXXXX) || fail "$1" mktemp
  IBMW_SHM_ROOT=$SHM_ROOT
  IBMW_SHM_DOMAIN="c$$$1"
  export IBMW_SHM_ROOT IBMW_SHM_DOMAIN
  service_dir="$SHM_ROOT/ibmw_shm.$(id -u).$IBMW_SHM_DOMAIN"
  [ "${#service_dir}" -le 63 ] || fail "$1" "service_dir_too_long:$service_dir"
}

has() { grep -q "$2" "$1" 2>/dev/null; }

assert_metrics() {
  mode=$1
  out="$WORK_DIR/$mode"
  [ -s "$out/metrics.jsonl" ] || fail "$mode" no_metrics_jsonl
  "$PYTHON" "$SCRIPT_DIR/assert_shm_metrics.py" --check-run "$mode" "$out/metrics.jsonl" \
    >"$out/assert.log" 2>&1 || fail "$mode" assert_shm_metrics
  echo "SHM_RETAINED_E2E_CHECK_PASS mode=$mode $(cat "$out/assert.log")"
}

# --- direct: single-process loan workload, the process is the metrics owner.
out="$WORK_DIR/direct"
rm -rf "$out"
mkdir -p "$out"
new_root direct
# Workloads run in the background and are waited on, so a TERM to this script
# interrupts the wait and cleanup signals them, rather than waiting for them to
# exit. The foreground commands are the 1 s sleep before the publisher starts
# and the short gate checks; a TERM during them is handled when they end.
IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$out" \
  timeout -s TERM -k 5 30 "$E2E" direct >"$out/direct.log" 2>&1 &
children=$!
wait "$children"
rc=$?
children=""
[ "$rc" -eq 0 ] || fail direct "exit_$rc"
has "$out/direct.log" 'DIRECT_LOAN_WORKLOAD .* pass=1' || fail direct no_pass_marker
! has "$out/direct.log" METRICS_FINAL_SNAPSHOT_FAIL || fail direct final_snapshot
assert_metrics direct

# --- retained: the subscriber is the sole metrics owner, the publisher has
# metrics off and never writes the artifact directory.
out="$WORK_DIR/retained"
rm -rf "$out"
mkdir -p "$out"
new_root retained
IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$out" \
  timeout -s TERM -k 5 60 "$E2E" subscriber >"$out/subscriber.log" 2>&1 &
sub=$!
children=$sub
sleep 1
IBMW_CORE_METRICS=off timeout -s TERM -k 5 60 "$E2E" publisher >"$out/publisher.log" 2>&1 &
pub=$!
children="$sub $pub"
wait "$pub"
pub_rc=$?
wait "$sub"
sub_rc=$?
children=""
[ "$pub_rc" -eq 0 ] || fail retained "publisher_exit_$pub_rc"
[ "$sub_rc" -eq 0 ] || fail retained "subscriber_exit_$sub_rc"
has "$out/publisher.log" 'RETAINED_E2E_PUBLISHER_DONE published=' || fail retained no_publisher_done
has "$out/subscriber.log" 'RETAINED_E2E_SUBSCRIBER_PASS' || fail retained no_subscriber_pass
has "$out/subscriber.log" 'RETAINED_E2E_CROSS_THREAD_RELEASE ok=1 payload_valid=1' || fail retained no_cross_thread_release
! has "$out/subscriber.log" METRICS_FINAL_SNAPSHOT_FAIL || fail retained final_snapshot
assert_metrics retained

cleanup
echo "SHM_RETAINED_E2E_CHECK_ALL_PASS"
