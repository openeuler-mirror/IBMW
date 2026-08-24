#!/bin/sh
set -eu
endpoint=${IBMW_ABC_ROS2_ENDPOINT:?set IBMW_ABC_ROS2_ENDPOINT}
artifact=${IBMW_ABC_LATENCY_JSONL:?set IBMW_ABC_LATENCY_JSONL}
topic=${IBMW_ABC_TOPIC:-/mw_perf/payload_4kb}
count=${IBMW_ABC_MESSAGE_COUNT:-3000}
warmup=${IBMW_ABC_WARMUP_MESSAGES:-50}
hz=${IBMW_ABC_HZ:-1000}
"$endpoint" --role pub --topic "$topic" --message-count "$count" --warmup-messages "$warmup" --hz "$hz" &
exec "$endpoint" --role sub --topic "$topic" --message-count "$count" --warmup-messages "$warmup" --jsonl "$artifact"
