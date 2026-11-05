#!/bin/sh
# shm counterpart of run_metrics_managed.sh: the same collector-managed leader
# protocol on shm_retained_loan_e2e. No RouDi: shm_ext is linked in. Every run
# uses a private discovery root under /tmp (never /dev/shm), removed on exit.
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 direct|retained BUILD_DIR METRICS_DIR" >&2
  exit 2
fi

MODE=$1
BUILD_DIR=$2
METRICS_DIR=$3
case "$MODE" in
  direct|retained) ;;
  *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac
E2E="$BUILD_DIR/shm_retained_loan_e2e"
if [ ! -x "$E2E" ]; then
  echo "shm_retained_loan_e2e is not executable: $E2E" >&2
  exit 2
fi
mkdir -p "$METRICS_DIR"
# Keep the previous inode allocated until this run creates its artifact. This
# makes same-valued deterministic counter snapshots distinguishable from stale
# files without weakening content-based stale checks.
rm -f "$METRICS_DIR/metrics.previous.jsonl"
if [ -f "$METRICS_DIR/metrics.jsonl" ]; then
  mv "$METRICS_DIR/metrics.jsonl" "$METRICS_DIR/metrics.previous.jsonl"
fi

SHM_ROOT=""
children=""
status=0

# Record a child's exit status. 0, and death by the HUP/INT/TERM a stop
# request sends (129/130/143), are expected; anything else is a workload
# failure and becomes the leader's exit status (the first one wins).
child_status() {
  case "$2" in
    0|129|130|143) ;;
    *)
      echo "shm_retained_loan_e2e $1 failed: exit status $2" >&2
      if [ "$status" -eq 0 ]; then status=$2; fi
      ;;
  esac
}

cleanup() {
  # mw-perf-collect signals the process group and then every live pid, so a
  # second TERM can arrive here: ignore it, or the root would not be removed.
  trap '' TERM INT HUP
  trap - EXIT
  for entry in $children; do kill -TERM "${entry#*:}" 2>/dev/null || true; done
  for entry in $children; do
    rc=0
    wait "${entry#*:}" 2>/dev/null || rc=$?
    child_status "${entry%%:*}" "$rc"
  done
  children=""
  rm -f "$METRICS_DIR/metrics.previous.jsonl"
  # Every user of the root has exited: nothing re-creates it any more.
  if [ -n "$SHM_ROOT" ]; then
    rm -rf "$SHM_ROOT"
  fi
  exit "$status"
}
# Installed before the root exists, so a TERM, INT or HUP cannot leave it
# behind (a SIGKILL of this shell still can: nothing runs then).
trap cleanup TERM INT HUP EXIT

# The service directory is <root>/ibmw_shm.<uid>.<domain>; it must fit the
# 63-byte limit of the library (socket path), the domain at most 32 chars.
# A stop request during this setup step exits with status 2.
status=2
SHM_ROOT=$(mktemp -d /tmp/ibR.XXXXXX)
IBMW_SHM_ROOT=$SHM_ROOT
IBMW_SHM_DOMAIN="rle$$"
export IBMW_SHM_ROOT IBMW_SHM_DOMAIN
service_dir="$SHM_ROOT/ibmw_shm.$(id -u).$IBMW_SHM_DOMAIN"
if [ "${#service_dir}" -gt 63 ]; then
  echo "shm service directory too long: $service_dir" >&2
  exit 2
fi
status=0

# children holds name:pid entries. Statuses are collected by cleanup, so a
# failing direct or subscriber workload is reported when the leader stops.
if [ "$MODE" = direct ]; then
  IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$METRICS_DIR" \
    MW_PERF_MANAGED=1 "$E2E" direct >"$METRICS_DIR/direct.log" 2>&1 &
  children="direct:$!"
else
  # The subscriber is the sole metrics owner. The publisher never writes the
  # exclusive artifact directory.
  IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$METRICS_DIR" \
    MW_PERF_MANAGED=1 "$E2E" subscriber >"$METRICS_DIR/subscriber.log" 2>&1 &
  children="subscriber:$!"
  sleep 1
  # In the background and waited on, so a stop request during publishing is
  # handled when it arrives (the trap exits; the || branch does not run). A
  # publisher failure stops the leader at once with the publisher's status.
  IBMW_CORE_METRICS=off "$E2E" publisher >"$METRICS_DIR/publisher.log" 2>&1 &
  pub=$!
  children="$children publisher:$pub"
  pub_rc=0
  wait "$pub" || pub_rc=$?
  children=${children% publisher:$pub}
  if [ "$pub_rc" -ne 0 ]; then
    child_status publisher "$pub_rc"
    status=$pub_rc
    exit "$pub_rc"
  fi
  # The subscriber remains live until collector TERM so /proc observations and
  # the reporter's final snapshot share one controlled shutdown boundary.
fi

# mw-perf-collect owns this managed leader and sends TERM after sampling. Keep
# the leader alive so collector shutdown is the graceful finalization boundary.
# The sleeps here and before the publisher run in the foreground: a stop
# request sent only to this shell is handled when the current sleep ends (at
# most 1 s); the collector's process-group TERM ends the sleep itself.
while :; do sleep 1; done
