#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR}"
BAGS_DIR="${BAGS_DIR:-$BUILD_DIR/convert_smoke_bags}"
CFG_DIR="${CFG_DIR:-$BUILD_DIR/convert_smoke_cfg}"
IBMW_BIN="${IBMW_BIN:-$BUILD_DIR/ibmw}"
[[ -x "$IBMW_BIN" ]] || IBMW_BIN="$BUILD_DIR/bin/ibmw"
RUN_DIR="$(mktemp -d "${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-convert-smoke}.XXXXXX")"
INPUT_BAG="$BAGS_DIR/ibmw_mcap"

fail() {
  echo "FAIL: convert smoke: $*" >&2
  echo "convert smoke artifacts: $RUN_DIR" >&2
  exit 1
}

[[ -x "$IBMW_BIN" ]] || fail "missing ibmw binary: $IBMW_BIN"
[[ -d "$INPUT_BAG" && -f "$INPUT_BAG/metadata.yaml" &&
   -f "$INPUT_BAG/data_0.mcap" ]] || fail "incomplete independent IBMW fixture"
for required_cfg in \
  convert_smoke_mcap_writer.yaml convert_smoke_mcap_reader.yaml \
  convert_smoke_sqlite_writer.yaml convert_smoke_sqlite_reader.yaml; do
  [[ -f "$CFG_DIR/$required_cfg" ]] || fail "missing independent config: $CFG_DIR/$required_cfg"
done

log_file() { printf '%s/%s.log' "$RUN_DIR" "$1"; }

run_convert() {
  local name="$1"
  shift
  "$IBMW_BIN" bag convert "$@" >"$(log_file "$name")" 2>&1
}

expect_convert_success() {
  local name="$1"
  shift
  if ! run_convert "$name" "$@"; then
    sed -n '1,240p' "$(log_file "$name")" >&2
    fail "$name should succeed"
  fi
}

expect_convert_failure() {
  local name="$1"
  local marker="$2"
  shift 2
  if run_convert "$name" "$@"; then
    sed -n '1,240p' "$(log_file "$name")" >&2
    fail "$name should fail"
  fi
  grep -Fq -- "$marker" "$(log_file "$name")" || {
    sed -n '1,240p' "$(log_file "$name")" >&2
    fail "$name error is missing: $marker"
  }
}

run_ibmw_info() {
  local name="$1"
  shift
  "$IBMW_BIN" bag info "$@" >"$(log_file "$name")" 2>&1
}

expect_ibmw_info() {
  local name="$1"
  shift
  if ! run_ibmw_info "$name" "$@"; then
    sed -n '1,240p' "$(log_file "$name")" >&2
    fail "$name info should succeed"
  fi
}

run_ros2_info() {
  local name="$1"
  shift
  ros2 bag info "$@" >"$(log_file "$name")" 2>&1
}

expect_ros2_info() {
  local name="$1"
  shift
  if ! run_ros2_info "$name" "$@"; then
    sed -n '1,240p' "$(log_file "$name")" >&2
    fail "$name ros2 bag info should succeed"
  fi
}

require_output() {
  local name="$1"
  local marker="$2"
  grep -Fq -- "$marker" "$(log_file "$name")" ||
    fail "$name output is missing: $marker"
}

require_metadata() {
  local bag="$1"
  local marker="$2"
  grep -Fq -- "$marker" "$bag/metadata.yaml" ||
    fail "$bag metadata is missing: $marker"
}

new_output() {
  local path="$RUN_DIR/$1"
  [[ ! -e "$path" ]] || fail "test output already exists: $path"
  printf '%s' "$path"
}

echo "convert smoke: help and independent input"
expect_convert_success help --help
require_output help "Convert:"
require_output help "--input-format <format>"
expect_convert_success help_short -h
require_output help_short "ibmw bag convert <input_bag>"

expect_ibmw_info input_info "$INPUT_BAG"
require_output input_info "Storage id:        mcap"
require_output input_info "Messages:          5"
require_output input_info "Topic: /sequence_probe"
require_output input_info "Complete playback: yes"

echo "convert smoke: IBMW to IBMW"
IBMW_SQLITE="$(new_output 01_ibmw_mcap_to_sqlite3)"
expect_convert_success ibmw_to_sqlite \
  "$INPUT_BAG" -o "$IBMW_SQLITE" \
  --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage sqlite3 \
  --input-storage-config-file "$CFG_DIR/convert_smoke_mcap_reader.yaml" \
  --storage-config-file "$CFG_DIR/convert_smoke_sqlite_writer.yaml"
expect_ibmw_info ibmw_to_sqlite_info --storage=sqlite3 "$IBMW_SQLITE"
require_output ibmw_to_sqlite_info "Storage id:        sqlite3"
require_output ibmw_to_sqlite_info "Messages:          5"
require_output ibmw_to_sqlite_info "Topic: /sequence_probe"
require_output ibmw_to_sqlite_info "    Type: ros2:std_msgs/msg/String"
require_output ibmw_to_sqlite_info "    Serialization: ros2"
require_output ibmw_to_sqlite_info "Complete playback: yes"

IBMW_MCAP="$(new_output 02_ibmw_sqlite3_to_mcap)"
expect_convert_success sqlite_to_ibmw \
  -i "$IBMW_SQLITE" --output="$IBMW_MCAP" \
  --input-format=ibmw --output-format=ibmw \
  --input-storage=sqlite3 --storage=mcap \
  --input-storage-config-file "$CFG_DIR/convert_smoke_sqlite_reader.yaml" \
  --storage-config-file "$CFG_DIR/convert_smoke_mcap_writer.yaml"
expect_ibmw_info sqlite_to_ibmw_info -s mcap "$IBMW_MCAP"
require_output sqlite_to_ibmw_info "Storage id:        mcap"
require_output sqlite_to_ibmw_info "Messages:          5"
require_output sqlite_to_ibmw_info "Topic: /sequence_probe"
require_output sqlite_to_ibmw_info "Complete playback: yes"

echo "convert smoke: IBMW to rosbag2"
ROS2_MCAP="$(new_output 03_ibmw_to_ros2_mcap)"
expect_convert_success ibmw_to_ros2_mcap \
  "$INPUT_BAG" -o "$ROS2_MCAP" \
  --input-format=auto --output-format=rosbag2 \
  --input-storage mcap --storage mcap
expect_ros2_info ibmw_to_ros2_mcap_info "$ROS2_MCAP"
require_output ibmw_to_ros2_mcap_info "Storage id:        mcap"
require_output ibmw_to_ros2_mcap_info "Messages:          5"
require_output ibmw_to_ros2_mcap_info "Topic: /sequence_probe | Type: std_msgs/msg/String | Count: 5 | Serialization Format: cdr"
require_metadata "$ROS2_MCAP" "rosbag2_bagfile_information:"

ROS2_SQLITE="$(new_output 04_ibmw_to_ros2_sqlite3)"
expect_convert_success ibmw_to_ros2_sqlite \
  "$INPUT_BAG" --output "$ROS2_SQLITE" \
  --input-format ibmw --output-format rosbag2 \
  --input-storage=mcap --storage=sqlite3
expect_ros2_info ibmw_to_ros2_sqlite_info -s sqlite3 "$ROS2_SQLITE"
require_output ibmw_to_ros2_sqlite_info "Storage id:        sqlite3"
require_output ibmw_to_ros2_sqlite_info "Messages:          5"
require_output ibmw_to_ros2_sqlite_info "Topic: /sequence_probe | Type: std_msgs/msg/String | Count: 5 | Serialization Format: cdr"
require_metadata "$ROS2_SQLITE" "rosbag2_bagfile_information:"

echo "convert smoke: rosbag2 to IBMW"
ROS2_MCAP_TO_IBMW="$(new_output 05_ros2_mcap_to_ibmw_sqlite3)"
expect_convert_success ros2_mcap_to_ibmw \
  "$ROS2_MCAP" -o "$ROS2_MCAP_TO_IBMW" \
  --input-format rosbag2 --output-format ibmw \
  --input-storage mcap --storage sqlite3
expect_ibmw_info ros2_mcap_to_ibmw_info "$ROS2_MCAP_TO_IBMW"
require_output ros2_mcap_to_ibmw_info "Storage id:        sqlite3"
require_output ros2_mcap_to_ibmw_info "Messages:          5"
require_output ros2_mcap_to_ibmw_info "Topic: /sequence_probe"
require_output ros2_mcap_to_ibmw_info "    Type: ros2:std_msgs/msg/String"
require_output ros2_mcap_to_ibmw_info "    Serialization: ros2"
require_output ros2_mcap_to_ibmw_info "Complete playback: yes"

ROS2_SQLITE_TO_IBMW="$(new_output 06_ros2_sqlite3_to_ibmw_mcap)"
expect_convert_success ros2_sqlite_to_ibmw \
  -i "$ROS2_SQLITE" -o "$ROS2_SQLITE_TO_IBMW" \
  --input-format=rosbag2 --output-format=ibmw \
  --input-storage=sqlite3 --storage=mcap
expect_ibmw_info ros2_sqlite_to_ibmw_info "$ROS2_SQLITE_TO_IBMW"
require_output ros2_sqlite_to_ibmw_info "Storage id:        mcap"
require_output ros2_sqlite_to_ibmw_info "Messages:          5"
require_output ros2_sqlite_to_ibmw_info "Topic: /sequence_probe"
require_output ros2_sqlite_to_ibmw_info "Complete playback: yes"

echo "convert smoke: split and compression options"
SPLIT_SIZE="$(new_output 07_split_by_size)"
expect_convert_success split_size \
  "$INPUT_BAG" -o "$SPLIT_SIZE" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage mcap --max-bag-size 300
[[ "$(find "$SPLIT_SIZE" -maxdepth 1 -name '*.mcap' | wc -l)" -ge 2 ]] ||
  fail "max-bag-size did not create multiple MCAP files"
require_metadata "$SPLIT_SIZE" "split_reason: size"
expect_ibmw_info split_size_info "$SPLIT_SIZE"
require_output split_size_info "Messages:          5"
require_output split_size_info "Complete playback: yes"

SPLIT_DURATION="$(new_output 08_split_by_duration)"
expect_convert_success split_duration \
  "$INPUT_BAG" -o "$SPLIT_DURATION" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage mcap --max-bag-duration=0.1
[[ "$(find "$SPLIT_DURATION" -maxdepth 1 -name '*.mcap' | wc -l)" -ge 2 ]] ||
  fail "max-bag-duration did not create multiple MCAP files"
require_metadata "$SPLIT_DURATION" "split_reason: duration"
expect_ibmw_info split_duration_info "$SPLIT_DURATION"
require_output split_duration_info "Messages:          5"
require_output split_duration_info "Complete playback: yes"

MESSAGE_ZSTD="$(new_output 09_message_zstd)"
expect_convert_success message_zstd \
  "$INPUT_BAG" -o "$MESSAGE_ZSTD" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage mcap --compression-mode message \
  --compression-format zstd --compression-queue-size 2 --compression-threads=2 \
  --storage-preset-profile small --storage-config-file "$CFG_DIR/convert_smoke_mcap_writer.yaml"
require_metadata "$MESSAGE_ZSTD" "compression_mode: message"
require_metadata "$MESSAGE_ZSTD" "compression_format: zstd"
require_metadata "$MESSAGE_ZSTD" "compression_queue_size: 2"
require_metadata "$MESSAGE_ZSTD" "compression_threads: 2"
require_metadata "$MESSAGE_ZSTD" "compression: zstd"
expect_ibmw_info message_zstd_info "$MESSAGE_ZSTD"
require_output message_zstd_info "Compression:       message/zstd"
require_output message_zstd_info "Messages:          5"
require_output message_zstd_info "Complete playback: yes"

MESSAGE_ZSTD_AUTO_THREADS="$(new_output 10_message_zstd_auto_threads)"
expect_convert_success message_zstd_auto_threads \
  "$INPUT_BAG" -o "$MESSAGE_ZSTD_AUTO_THREADS" --input-format ibmw \
  --output-format ibmw --input-storage mcap --storage mcap \
  --compression-mode=message --compression-format=zstd \
  --compression-queue-size=2 --compression-threads=0
require_metadata "$MESSAGE_ZSTD_AUTO_THREADS" "compression_mode: message"
require_metadata "$MESSAGE_ZSTD_AUTO_THREADS" "compression_format: zstd"
grep -Eq '^  compression_threads: [1-9][0-9]*$' \
  "$MESSAGE_ZSTD_AUTO_THREADS/metadata.yaml" ||
  fail "compression-threads=0 did not resolve to a positive worker count"
expect_ibmw_info message_zstd_auto_threads_info "$MESSAGE_ZSTD_AUTO_THREADS"
require_output message_zstd_auto_threads_info "Messages:          5"
require_output message_zstd_auto_threads_info "Complete playback: yes"

FILE_LZ4="$(new_output 10_file_lz4)"
expect_convert_success file_lz4 \
  "$INPUT_BAG" -o "$FILE_LZ4" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage mcap --compression-mode=file \
  --compression-format=lz4 --compression-queue-size 2 --compression-threads 2
[[ -f "$FILE_LZ4/data_0.mcap.lz4" ]] || fail "file compression did not create .mcap.lz4"
require_metadata "$FILE_LZ4" "compression_mode: file"
require_metadata "$FILE_LZ4" "compression_format: lz4"
require_metadata "$FILE_LZ4" "compression: lz4"
expect_ibmw_info file_lz4_info "$FILE_LZ4"
require_output file_lz4_info "Compression:       file/lz4"
require_output file_lz4_info "Messages:          5"
require_output file_lz4_info "Complete playback: yes"

FASTWRITE="$(new_output 11_mcap_fastwrite)"
expect_convert_success mcap_fastwrite \
  "$INPUT_BAG" -o "$FASTWRITE" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage mcap --storage-preset-profile fastwrite
expect_ibmw_info mcap_fastwrite_info "$FASTWRITE"
require_output mcap_fastwrite_info "Messages:          5"
require_output mcap_fastwrite_info "Complete playback: yes"

RESILIENT="$(new_output 12_sqlite_resilient)"
expect_convert_success sqlite_resilient \
  "$INPUT_BAG" -o "$RESILIENT" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage sqlite3 --storage-preset-profile resilient
expect_ibmw_info sqlite_resilient_info "$RESILIENT"
require_output sqlite_resilient_info "Storage id:        sqlite3"
require_output sqlite_resilient_info "Messages:          5"
require_output sqlite_resilient_info "Complete playback: yes"

PERFORMANCE="$(new_output 13_sqlite_performance)"
expect_convert_success sqlite_performance \
  "$INPUT_BAG" -o "$PERFORMANCE" --input-format ibmw --output-format ibmw \
  --input-storage mcap --storage sqlite3 --storage-preset-profile performance
expect_ibmw_info sqlite_performance_info "$PERFORMANCE"
require_output sqlite_performance_info "Storage id:        sqlite3"
require_output sqlite_performance_info "Messages:          5"
require_output sqlite_performance_info "Complete playback: yes"

echo "convert smoke: invalid CLI and unsupported combinations"
expect_convert_failure missing_input "requires an input bag" \
  -o "$RUN_DIR/invalid_missing_input"
expect_convert_failure missing_output "requires --output" "$INPUT_BAG"
expect_convert_failure missing_path "does not exist" \
  "$BAGS_DIR/missing" -o "$RUN_DIR/invalid_missing_path"
expect_convert_failure same_path "must be different paths" \
  "$INPUT_BAG" -o "$INPUT_BAG"
expect_convert_failure unknown_input_format "Input format must be" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_format" --input-format unknown
expect_convert_failure unknown_output_format "Output format must be" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_format2" --output-format unknown
expect_convert_failure wrong_input_storage "Requested storage" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_storage" --input-storage sqlite3
expect_convert_failure compression_sqlite "Compression is only supported for MCAP" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_compression" --storage sqlite3 \
  --compression-mode message --compression-format zstd
expect_convert_failure zero_queue "Compression queue size cannot be zero" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_queue" --compression-mode message \
  --compression-format zstd --compression-queue-size 0
expect_convert_failure missing_config "Output storage config file does not exist" \
  "$INPUT_BAG" -o "$RUN_DIR/invalid_config" \
  --storage-config-file "$RUN_DIR/missing.yaml"

echo "PASS: bag convert smoke conversion and CLI matrix"
echo "convert smoke artifacts: $RUN_DIR"
