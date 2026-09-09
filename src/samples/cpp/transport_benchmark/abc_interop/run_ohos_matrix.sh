#!/usr/bin/env bash
set -euo pipefail

HDC=${HDC:-hdc}
IBMW_ABC_ROOT=${IBMW_ABC_ROOT:-/data/local/tmp/ibmw-ohos-abc}
MATRIX_ID=${MATRIX_ID:-ohos-ab-5block-$(date +%Y%m%d-%H%M%S)}
BLOCKS=${BLOCKS:-5}
COUNT=${COUNT:-3000}
COOLDOWN_SECONDS=${COOLDOWN_SECONDS:-3}
device_runner="$IBMW_ABC_ROOT/run_ohos_lane.sh"

[[ "$BLOCKS" =~ ^[1-9][0-9]*$ ]] || { printf 'Invalid BLOCKS: %s\n' "$BLOCKS" >&2; exit 2; }
[[ "$COUNT" =~ ^[1-9][0-9]*$ ]] || { printf 'Invalid COUNT: %s\n' "$COUNT" >&2; exit 2; }
[[ "$COOLDOWN_SECONDS" =~ ^[0-9]+$ ]] || { printf 'Invalid COOLDOWN_SECONDS: %s\n' "$COOLDOWN_SECONDS" >&2; exit 2; }
[[ "$MATRIX_ID" =~ ^[A-Za-z0-9._-]+$ ]] || { printf 'Invalid MATRIX_ID: %s\n' "$MATRIX_ID" >&2; exit 2; }
[[ "$IBMW_ABC_ROOT" =~ ^/[A-Za-z0-9_./-]+$ ]] || { printf 'Invalid IBMW_ABC_ROOT: %s\n' "$IBMW_ABC_ROOT" >&2; exit 2; }

for ((block = 1; block <= BLOCKS; ++block)); do
  if ((block % 2 == 1)); then
    lanes=(A B)
  else
    lanes=(B A)
  fi
  for lane in "${lanes[@]}"; do
    run_id="${MATRIX_ID}-block${block}-${lane}"
    "$HDC" shell "IBMW_ABC_ROOT='$IBMW_ABC_ROOT' sh '$device_runner' '$lane' '$run_id' '$COUNT'"
    sleep "$COOLDOWN_SECONDS"
  done
done
