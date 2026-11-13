#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

INPUT_BAG="./bag_convert_demo_input/ibmw_mcap"
OUTPUT_ROOT="${1:-./bag_convert_demo_output}"

for required_file in \
  ./ibmw \
  ./libibmw_record_playback.so \
  "${INPUT_BAG}/metadata.yaml" \
  "${INPUT_BAG}/data_0.mcap"; do
  if [[ ! -e "${required_file}" ]]; then
    echo "bag_convert_demo: required fresh-build artifact is missing: ${required_file}" >&2
    exit 1
  fi
done

if ! command -v ros2 >/dev/null 2>&1; then
  echo "bag_convert_demo: ros2 is not available; source the ROS2 environment first." >&2
  exit 1
fi
ROS2_STORAGE_PLUGINS="$(ros2 bag list storage 2>/dev/null)"
if ! grep -Fx mcap <<< "${ROS2_STORAGE_PLUGINS}" >/dev/null; then
  echo "bag_convert_demo: the rosbag2 MCAP storage plugin is not available." >&2
  echo "Install ros-${ROS_DISTRO:-humble}-rosbag2-storage-mcap and source ROS2 again." >&2
  exit 1
fi

if [[ -e "${OUTPUT_ROOT}" ]]; then
  echo "bag_convert_demo: output path already exists: ${OUTPUT_ROOT}" >&2
  echo "Choose a new path, for example: ./run_bag_convert_demo.sh ./bag_convert_demo_output_2" >&2
  exit 1
fi
mkdir -p "${OUTPUT_ROOT}"

IBMW_SQLITE="${OUTPUT_ROOT}/01_ibmw_sqlite3"
IBMW_MCAP_ROUNDTRIP="${OUTPUT_ROOT}/02_ibmw_mcap_roundtrip"
ROS2_MCAP="${OUTPUT_ROOT}/03_ros2_mcap"
ROS2_SQLITE="${OUTPUT_ROOT}/04_ros2_sqlite3"
FROM_ROS2_MCAP="${OUTPUT_ROOT}/05_ros2_mcap_to_ibmw_sqlite3"
FROM_ROS2_SQLITE="${OUTPUT_ROOT}/06_ros2_sqlite3_to_ibmw_mcap"

echo "[1/3] IBMW internal conversion: MCAP -> SQLite3 -> MCAP"
./ibmw bag convert "${INPUT_BAG}" \
  -o "${IBMW_SQLITE}" \
  --input-format ibmw \
  --output-format ibmw \
  --input-storage mcap \
  --storage sqlite3
./ibmw bag convert "${IBMW_SQLITE}" \
  -o "${IBMW_MCAP_ROUNDTRIP}" \
  --input-format ibmw \
  --output-format ibmw \
  --input-storage sqlite3 \
  --storage mcap

echo
echo "[2/3] IBMW conversion to rosbag2: MCAP and SQLite3"
./ibmw bag convert "${INPUT_BAG}" \
  -o "${ROS2_MCAP}" \
  --input-format ibmw \
  --output-format rosbag2 \
  --input-storage mcap \
  --storage mcap
./ibmw bag convert "${INPUT_BAG}" \
  -o "${ROS2_SQLITE}" \
  --input-format ibmw \
  --output-format rosbag2 \
  --input-storage mcap \
  --storage sqlite3

echo
echo "[3/3] rosbag2 conversion to IBMW: MCAP and SQLite3 inputs"
./ibmw bag convert "${ROS2_MCAP}" \
  -o "${FROM_ROS2_MCAP}" \
  --input-format rosbag2 \
  --output-format ibmw \
  --input-storage mcap \
  --storage sqlite3
./ibmw bag convert "${ROS2_SQLITE}" \
  -o "${FROM_ROS2_SQLITE}" \
  --input-format rosbag2 \
  --output-format ibmw \
  --input-storage sqlite3 \
  --storage mcap

echo
echo "========== IBMW input =========="
./ibmw bag info "${INPUT_BAG}"
echo
echo "========== IBMW internal: SQLite3 =========="
./ibmw bag info "${IBMW_SQLITE}"
echo
echo "========== IBMW internal: MCAP round trip =========="
./ibmw bag info "${IBMW_MCAP_ROUNDTRIP}"
echo
echo "========== ROS2 output: MCAP =========="
ros2 bag info "${ROS2_MCAP}"
echo
echo "========== ROS2 output: SQLite3 =========="
ros2 bag info "${ROS2_SQLITE}"
echo
echo "========== ROS2 MCAP -> IBMW SQLite3 =========="
./ibmw bag info "${FROM_ROS2_MCAP}"
echo
echo "========== ROS2 SQLite3 -> IBMW MCAP =========="
./ibmw bag info "${FROM_ROS2_SQLITE}"

echo
echo "bag_convert_demo completed. All outputs are under ${OUTPUT_ROOT}."
