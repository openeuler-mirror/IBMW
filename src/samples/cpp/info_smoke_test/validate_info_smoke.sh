#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR}"
BAGS_DIR="${BAGS_DIR:-$BUILD_DIR/info_smoke_bags}"
IBMW_BIN="${IBMW_BIN:-$BUILD_DIR/ibmw}"
[[ -x "$IBMW_BIN" ]] || IBMW_BIN="$BUILD_DIR/bin/ibmw"
RUN_DIR="$(mktemp -d "${TMP_DIR:-${TMPDIR:-/tmp}/ibmw-info-smoke}.XXXXXX")"

MCAP_BAG="$BAGS_DIR/mcap"
SQLITE_BAG="$BAGS_DIR/sqlite3"
MULTI_FILE_BAG="$BAGS_DIR/multi_file_mcap"
MESSAGE_ZSTD_BAG="$BAGS_DIR/message_zstd_mcap"
FILE_LZ4_BAG="$BAGS_DIR/file_lz4_mcap"
EMPTY_BAG="$BAGS_DIR/empty_mcap"

fail() {
  echo "FAIL: info smoke: $*" >&2
  echo "info smoke artifacts: $RUN_DIR" >&2
  exit 1
}

[[ -x "$IBMW_BIN" ]] || fail "missing ibmw binary: $IBMW_BIN"
[[ -d "$MCAP_BAG" ]] || fail "missing MCAP fixture: $MCAP_BAG"
[[ -d "$SQLITE_BAG" ]] || fail "missing SQLite3 fixture: $SQLITE_BAG"
[[ -d "$MULTI_FILE_BAG" ]] || fail "missing multi-file fixture: $MULTI_FILE_BAG"
[[ -d "$MESSAGE_ZSTD_BAG" ]] || fail "missing zstd fixture: $MESSAGE_ZSTD_BAG"
[[ -d "$FILE_LZ4_BAG" ]] || fail "missing lz4 fixture: $FILE_LZ4_BAG"
[[ -d "$EMPTY_BAG" ]] || fail "missing empty fixture: $EMPTY_BAG"
[[ -f "$MCAP_BAG/metadata.yaml" && -f "$MCAP_BAG/data_0.mcap" ]] ||
  fail "MCAP fixture is incomplete"
[[ -f "$SQLITE_BAG/metadata.yaml" && -f "$SQLITE_BAG/data_0.db3" ]] ||
  fail "SQLite3 fixture is incomplete"

run_info() {
  local name="$1"
  shift
  "$IBMW_BIN" bag info "$@" >"$RUN_DIR/$name.log" 2>&1
}

expect_info_success() {
  local name="$1"
  shift
  if ! run_info "$name" "$@"; then
    sed -n '1,220p' "$RUN_DIR/$name.log" >&2
    fail "$name should succeed"
  fi
}

expect_info_failure() {
  local name="$1"
  shift
  if run_info "$name" "$@"; then
    sed -n '1,220p' "$RUN_DIR/$name.log" >&2
    fail "$name should fail"
  fi
}

require_output() {
  local name="$1"
  local marker="$2"
  grep -Fq -- "$marker" "$RUN_DIR/$name.log" ||
    fail "$name output is missing: $marker"
}

require_output_count() {
  local name="$1"
  local pattern="$2"
  local expected="$3"
  local actual
  actual="$(grep -Ec -- "$pattern" "$RUN_DIR/$name.log" || true)"
  [[ "$actual" == "$expected" ]] ||
    fail "$name has $actual matches for '$pattern', expected $expected"
}

echo "info smoke: help"
expect_info_success help --help
require_output help "Info:"
require_output help "-s, --storage <id>"
expect_info_success help_short -h
require_output help_short "ibmw bag info <bag_directory>"

check_common_bag_info() {
  local name="$1"
  local storage="$2"
  local size="$3"
  require_output "$name" "Storage id:        $storage"
  require_output "$name" "Files:             1"
  require_output "$name" "Bag size:          $size B"
  require_output "$name" "Bag version:       1"
  require_output "$name" "Duration:          4.000499228s"
  require_output "$name" "Start:             1786679571965552151 ns"
  require_output "$name" "End:               1786679575966051379 ns"
  require_output "$name" "Messages:          5"
  require_output "$name" "Closed cleanly:    yes"
  require_output "$name" "Dropped messages:  0"
  require_output "$name" "Duplicate copies:  0"
  require_output "$name" "Complete playback: yes"
  require_output "$name" "Topic: playback_demo_topic"
  require_output "$name" "    Type: dds:ibmw::protocols::common::Header"
  require_output "$name" "    Messages: 5"
  require_output "$name" "    Serialization: cdr"
  require_output "$name" "ibmw::protocols::common::Header encoding=omgidl hash=fnv1a64:31cc1f944e68859f size=472 valid=yes"
  require_output "$name" "      depth: 10"
  require_output "$name" "      durability: volatile"
  require_output "$name" "      history: keep_last"
  require_output "$name" "      reliability: reliable"
  require_output_count "$name" '^  Topic: ' 1
  require_output_count "$name" '^  [^ ]+  size=' 1
}

echo "info smoke: MCAP default and explicit storage"
expect_info_success mcap_default "$MCAP_BAG"
check_common_bag_info mcap_default mcap 2453
require_output mcap_default "data_0.mcap  size="
require_output mcap_default "compression=none"

expect_info_success mcap_explicit --storage mcap "$MCAP_BAG"
check_common_bag_info mcap_explicit mcap 2453

expect_info_success mcap_short -s mcap "$MCAP_BAG"
check_common_bag_info mcap_short mcap 2453

echo "info smoke: SQLite3 default and explicit storage"
expect_info_success sqlite_default "$SQLITE_BAG"
check_common_bag_info sqlite_default sqlite3 24576
require_output sqlite_default "data_0.db3  size="
require_output sqlite_default "compression=none"

expect_info_success sqlite_explicit --storage=sqlite3 "$SQLITE_BAG"
check_common_bag_info sqlite_explicit sqlite3 24576

echo "info smoke: multi-file and compressed MCAP"
expect_info_success multi_file "$MULTI_FILE_BAG"
require_output multi_file "Files:             2"
require_output multi_file "Messages:          19"
require_output multi_file "data_0.mcap  size="
require_output multi_file "data_1.mcap  size="
require_output multi_file "split_reason=size"
require_output multi_file "    Messages: 19"
require_output multi_file "Complete playback: yes"
require_output_count multi_file '^  [^ ]+  size=' 2

expect_info_success message_zstd "$MESSAGE_ZSTD_BAG"
require_output message_zstd "Compression:       message/zstd"
require_output message_zstd "data_0.mcap  size="
require_output message_zstd "compression=zstd"
require_output message_zstd "Messages:          13"
require_output message_zstd "Complete playback: yes"

expect_info_success file_lz4 "$FILE_LZ4_BAG"
require_output file_lz4 "Compression:       file/lz4"
require_output file_lz4 "data_0.mcap.lz4  size="
require_output file_lz4 "compression=lz4"
require_output file_lz4 "Messages:          13"
require_output file_lz4 "Complete playback: yes"

echo "info smoke: empty MCAP"
expect_info_success empty "$EMPTY_BAG"
require_output empty "Files:             1"
require_output empty "Messages:          0"
require_output empty "Closed cleanly:    yes"
require_output empty "Complete playback: yes"
require_output_count empty '^  Topic: ' 0

echo "info smoke: invalid input and option handling"
expect_info_failure missing_bag_path
require_output missing_bag_path "bag info requires a bag path"
expect_info_failure storage_without_id --storage
require_output storage_without_id "requires a storage ID"
expect_info_failure unknown_storage --storage unknown "$MCAP_BAG"
require_output unknown_storage "does not match bag storage"
expect_info_failure wrong_storage --storage sqlite3 "$MCAP_BAG"
require_output wrong_storage "does not match bag storage"
expect_info_failure missing_path "$BAGS_DIR/does-not-exist"
require_output missing_path "does not exist"
expect_info_failure unsupported_option --not-an-info-option "$MCAP_BAG"
require_output unsupported_option "Unsupported bag info option"
expect_info_failure multiple_paths "$MCAP_BAG" "$SQLITE_BAG"
require_output multiple_paths "bag info accepts one bag path"

mkdir -p "$RUN_DIR/incomplete_bag"
cp "$MCAP_BAG/metadata.yaml" "$RUN_DIR/incomplete_bag/metadata.yaml"
expect_info_failure incomplete_bag "$RUN_DIR/incomplete_bag"
require_output incomplete_bag "does not exist"

echo "PASS: bag info smoke CLI and metadata matrix"
echo "info smoke artifacts: $RUN_DIR"
