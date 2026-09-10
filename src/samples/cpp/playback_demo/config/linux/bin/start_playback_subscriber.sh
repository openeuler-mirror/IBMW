#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

for required_file in \
  ./ibmw \
  ./libplayback_demo_pkg.so \
  ./libibmw_dds_transport.so \
  ./cfg/playback_demo_subscriber.yaml; do
  if [[ ! -e "${required_file}" ]]; then
    echo "playback_demo: required fresh-build artifact is missing: ${required_file}" >&2
    exit 1
  fi
done

exec ./ibmw run ./cfg/playback_demo_subscriber.yaml
