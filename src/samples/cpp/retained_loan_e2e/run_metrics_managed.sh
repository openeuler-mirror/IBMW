#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
  echo "usage: $0 direct|retained BUILD_DIR METRICS_DIR ROUDI_BIN" >&2
  exit 2
fi

MODE=$1
BUILD_DIR=$2
METRICS_DIR=$3
ROUDI_BIN=$4
E2E="$BUILD_DIR/retained_loan_e2e"
mkdir -p "$METRICS_DIR"
# Keep the previous inode allocated until this run creates its artifact. This
# makes same-valued deterministic counter snapshots distinguishable from stale
# files without weakening content-based stale checks.
rm -f "$METRICS_DIR/metrics.previous.jsonl"
if [ -f "$METRICS_DIR/metrics.jsonl" ]; then
  mv "$METRICS_DIR/metrics.jsonl" "$METRICS_DIR/metrics.previous.jsonl"
fi
children=""

cleanup() {
  trap - TERM INT EXIT
  for pid in $children; do kill -TERM "$pid" 2>/dev/null || true; done
  for pid in $children; do wait "$pid" 2>/dev/null || true; done
  rm -f "$METRICS_DIR/metrics.previous.jsonl"
  exit 0
}
trap cleanup TERM INT EXIT

"$ROUDI_BIN" >"$METRICS_DIR/roudi.log" 2>&1 &
children="$!"
sleep 1

if [ "$MODE" = direct ]; then
  IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$METRICS_DIR" \
    MW_PERF_MANAGED=1 "$E2E" direct &
  work=$!
  children="$work $children"
elif [ "$MODE" = retained ]; then
  # The subscriber is the sole metrics owner. Publisher and RouDi never write
  # the exclusive artifact directory.
  IBMW_CORE_METRICS=counters IBMW_OBSERVABILITY_OUTPUT_DIR="$METRICS_DIR" \
    MW_PERF_MANAGED=1 "$E2E" subscriber >"$METRICS_DIR/subscriber.log" 2>&1 &
  sub=$!
  children="$sub $children"
  sleep 1
  IBMW_CORE_METRICS=off "$E2E" publisher >"$METRICS_DIR/publisher.log" 2>&1
  # The subscriber remains live until collector TERM so /proc observations and
  # the reporter's final snapshot share one controlled shutdown boundary.
else
  echo "unknown mode: $MODE" >&2
  exit 2
fi

# mw-perf-collect owns this managed leader and sends TERM after sampling. Keep
# the leader alive so collector shutdown is the graceful finalization boundary.
while :; do sleep 1; done
