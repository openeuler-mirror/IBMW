#!/bin/sh

fail() {
  printf 'Invalid OHOS latency artifact: %s\n' "$1" >&2
  exit 1
}

[ "$#" -eq 5 ] || fail "usage: $0 ARTIFACT_JSONL COUNT RUN_ID PRODUCER_PID PRODUCER_START_TICKS"

artifact=$1
count=$2
run_id=$3
producer_pid=$4
producer_start_ticks=$5

case "$count" in ''|*[!0-9]*|0) fail "invalid count" ;; esac
case "$run_id" in ''|*[!A-Za-z0-9._-]*) fail "invalid run id" ;; esac
case "$producer_pid" in ''|*[!0-9]*|0) fail "invalid producer pid" ;; esac
case "$producer_start_ticks" in ''|*[!0-9]*|0) fail "invalid producer start ticks" ;; esac
[ -f "$artifact" ] && [ -s "$artifact" ] || fail "missing or empty JSONL file"

expected_lines=$((count + 1))
actual_lines=$(wc -l <"$artifact" 2>/dev/null) || fail "cannot count JSONL lines"
[ "$actual_lines" -eq "$expected_lines" ] || fail "expected $expected_lines lines, found $actual_lines"

awk -v count="$count" -v run_id="$run_id" -v producer_pid="$producer_pid" \
    -v producer_start_ticks="$producer_start_ticks" '
function bad(message) {
  print "Invalid OHOS latency artifact at line " NR ": " message > "/dev/stderr"
  exit 1
}
function consume(text) {
  if (index(rest, text) != 1) bad("malformed or unexpected field")
  rest = substr(rest, length(text) + 1)
}
function number_before(delimiter, value, delimiter_at) {
  delimiter_at = index(rest, delimiter)
  if (!delimiter_at) bad("missing numeric field delimiter")
  value = substr(rest, 1, delimiter_at - 1)
  if (value !~ /^(0|[1-9][0-9]*)$/) bad("invalid numeric field")
  rest = substr(rest, delimiter_at + length(delimiter))
  return value
}
function decimal_compare(left, right) {
  if (length(left) != length(right)) return length(left) < length(right) ? -1 : 1
  if ("x" left == "x" right) return 0
  return "x" left < "x" right ? -1 : 1
}
function decimal_subtract(left, right, result, borrow, idx, digit, right_index) {
  result = ""
  borrow = 0
  for (idx = length(left); idx > 0; --idx) {
    digit = substr(left, idx, 1) - borrow
    right_index = length(right) - length(left) + idx
    if (right_index > 0) digit -= substr(right, right_index, 1)
    if (digit < 0) { digit += 10; borrow = 1 } else borrow = 0
    result = digit result
  }
  sub(/^0+/, "", result)
  return result == "" ? "0" : result
}
function decimal_increment(value, result, carry, idx, digit) {
  result = ""
  carry = 1
  for (idx = length(value); idx > 0; --idx) {
    digit = substr(value, idx, 1) + carry
    if (digit == 10) { digit = 0; carry = 1 } else carry = 0
    result = digit result
  }
  return carry ? "1" result : result
}
BEGIN { expected_seq = "50" }
NR <= count {
  rest = $0
  consume("{\"schema\":\"mw_perf.latency.v1\",\"seq\":")
  seq = number_before(",\"send_ns\":")
  send_ns = number_before(",\"receive_ns\":")
  receive_ns = number_before(",\"latency_ns\":")
  latency_ns = number_before(",\"topic\":\"/mw_perf/payload_4kb\",\"type\":\"std_msgs/msg/UInt8MultiArray\",\"payload_bytes\":")
  payload_bytes = number_before(",\"valid\":true}")
  if (rest != "") bad("trailing or unexpected content")
  if ("x" seq != "x" expected_seq) bad("non-contiguous sequence")
  if (send_ns == "0") bad("send_ns must be positive")
  if (decimal_compare(receive_ns, send_ns) < 0) bad("receive_ns precedes send_ns")
  if ("x" latency_ns != "x" decimal_subtract(receive_ns, send_ns)) bad("latency arithmetic mismatch")
  if (payload_bytes != "4096") bad("unexpected payload size")
  expected_seq = decimal_increment(expected_seq)
  next
}
NR == count + 1 {
  expected = "{\"schema\":\"mw_perf.latency.v1\",\"kind\":\"completion\",\"run_id\":\"" run_id \
    "\",\"producer_pid\":" producer_pid ",\"producer_start_time_ticks\":" producer_start_ticks \
    ",\"record_count\":" count ",\"topic\":\"/mw_perf/payload_4kb\",\"type\":\"std_msgs/msg/UInt8MultiArray\"" \
    ",\"complete\":true,\"clock_domain\":\"producer_monotonic\"}"
  if ($0 != expected) bad("invalid completion record")
  completion = 1
  next
}
{ bad("unexpected extra record") }
END {
  if (NR != count + 1 || !completion) exit 1
}
' "$artifact" || fail "content validation failed"
