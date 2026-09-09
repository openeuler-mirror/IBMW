#!/usr/bin/env bash
set -euo pipefail
HDC=${HDC:-hdc}
ROOT=${IBMW_TOPOLOGY_ROOT:-/data/local/tmp/ibmw-ohos-topology}
MATRIX_ID=${MATRIX_ID:-topology-ohos-$(date +%Y%m%d-%H%M%S)}
BLOCKS=${BLOCKS:-5}
COUNT=${COUNT:-3000}
for ((block=1; block<=BLOCKS; ++block)); do
  if ((block % 2)); then lanes=(A B); else lanes=(B A); fi
  for lane in "${lanes[@]}"; do
    run_id="$MATRIX_ID-block$block-$lane"
    output=$("$HDC" shell "IBMW_TOPOLOGY_ROOT='$ROOT' sh '$ROOT/run_ohos_topology_lane.sh' '$lane' '$run_id' '$COUNT'")
    printf '%s\n' "$output"
    grep -q "lane=$lane run_id=$run_id records_per_sink=$COUNT valid=true" <<<"$output" || exit 1
    sleep 15
  done
done
