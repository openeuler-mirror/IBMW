#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

source "${SCRIPT_DIR}/record_demo_env.sh"
RUNTIME_DIR="${XDG_RUNTIME_DIR:+${XDG_RUNTIME_DIR}/ibmw}"
RUNTIME_DIR="${RUNTIME_DIR:-/tmp/ibmw-$(id -u)}"

for required_file in \
  ./ibmw \
  ./iox-roudi \
  ./librecord_demo_pkg.so \
  ./libibmw_dds_transport.so \
  ./libibmw_iceoryx_transport.so \
  ./libibmw_mqtt_transport.so \
  ./libibmw_ros2_transport.so \
  ./libibmw_record_playback.so \
  ./libcommon_dds_ts.so \
  ./libexample_ros2_ros2_ts.so; do
  if [[ ! -e "${required_file}" ]]; then
    echo "record_demo: required fresh-build artifact is missing: ${required_file}" >&2
    exit 1
  fi
done

for command_name in mosquitto mosquitto_pub pgrep ss; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "record_demo: required command '${command_name}' is not installed." >&2
    exit 1
  fi
done

declare -a CHILDREN=()
declare -A CHILD_PIDS=()
ROUDI_PID=""
MOSQUITTO_PID=""

cleanup() {
  trap - EXIT INT TERM
  for pid in "${CHILDREN[@]}"; do
    kill -TERM "${pid}" 2>/dev/null || true
  done
  for pid in "${CHILDREN[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done
  if [[ -n "${ROUDI_PID}" ]]; then
    kill -TERM "${ROUDI_PID}" 2>/dev/null || true
    wait "${ROUDI_PID}" 2>/dev/null || true
  fi
  if [[ -n "${MOSQUITTO_PID}" ]]; then
    kill -TERM "${MOSQUITTO_PID}" 2>/dev/null || true
    wait "${MOSQUITTO_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if ! pgrep -x iox-roudi >/dev/null 2>&1; then
  ./iox-roudi > /tmp/ibmw-record-demo-roudi.log 2>&1 &
  ROUDI_PID=$!
  sleep 1
  if ! kill -0 "${ROUDI_PID}" 2>/dev/null; then
    echo "record_demo: fresh-build ./iox-roudi failed to start; see /tmp/ibmw-record-demo-roudi.log." >&2
    exit 1
  fi
fi

if ! ss -ltn '( sport = :1884 )' 2>/dev/null | tail -n +2 | grep -q .; then
  mosquitto -p 1884 -v > /tmp/ibmw-record-demo-mosquitto.log 2>&1 &
  MOSQUITTO_PID=$!
  sleep 1
  if ! kill -0 "${MOSQUITTO_PID}" 2>/dev/null; then
    echo "record_demo: mosquitto failed to listen on 127.0.0.1:1884; see /tmp/ibmw-record-demo-mosquitto.log." >&2
    exit 1
  fi
fi
if ! mosquitto_pub -h 127.0.0.1 -p 1884 \
     -t /record_demo/startup_check -m ready -q 1; then
  echo "record_demo: port 1884 does not provide a usable local MQTT Broker." >&2
  exit 1
fi

for cfg in \
  ./cfg/record_demo_dds.yaml \
  ./cfg/record_demo_iceoryx.yaml \
  ./cfg/record_demo_mqtt.yaml \
  ./cfg/record_demo_ros2.yaml; do
  echo "Starting ${cfg}"
  ./ibmw run "${cfg}" > "/tmp/$(basename "${cfg%.yaml}").log" 2>&1 &
  child_pid=$!
  CHILDREN+=("${child_pid}")
  case "${cfg}" in
    ./cfg/record_demo_dds.yaml) CHILD_PIDS[record-demo-dds]="${child_pid}" ;;
    ./cfg/record_demo_iceoryx.yaml) CHILD_PIDS[record-demo-iceoryx]="${child_pid}" ;;
    ./cfg/record_demo_mqtt.yaml) CHILD_PIDS[record-demo-mqtt]="${child_pid}" ;;
    ./cfg/record_demo_ros2.yaml) CHILD_PIDS[record-demo-ros2]="${child_pid}" ;;
  esac
done

# A Runtime must finish module initialization before the recorder takes its
# first graph snapshot.  The registry files are the synchronization point.
for runtime_id in record-demo-dds record-demo-iceoryx record-demo-mqtt record-demo-ros2; do
  registration="$RUNTIME_DIR/v2/${runtime_id}.json"
  child_pid="${CHILD_PIDS[${runtime_id}]}"
  for attempt in {1..50}; do
    if [[ -s "${registration}" ]] &&
       grep -Eq '"pid"[[:space:]]*:[[:space:]]*'"${child_pid}"'([,[:space:]]|$)' "${registration}" &&
       kill -0 "${child_pid}" 2>/dev/null; then
      break
    fi
    if ! kill -0 "${child_pid}" 2>/dev/null; then
      echo "record_demo: publisher for Runtime Agent ${runtime_id} exited before registration." >&2
      exit 1
    fi
    sleep 0.1
  done
  if [[ ! -s "${registration}" ]] ||
     ! grep -Eq '"pid"[[:space:]]*:[[:space:]]*'"${child_pid}"'([,[:space:]]|$)' "${registration}" ||
     ! kill -0 "${child_pid}" 2>/dev/null; then
    echo "record_demo: Runtime Agent ${runtime_id} did not register." >&2
    exit 1
  fi
done

echo
echo "Four record_demo publishers are running."
echo "Run recording in another terminal:"
echo "  ./ibmw bag record record_demo_dds_topic record_demo_iceoryx_topic record_demo_mqtt_topic record_demo_ros2_topic -o record_demo_bag"
echo "Then run: ./ibmw bag info record_demo_bag"
echo "Publisher logs: /tmp/record_demo_*.log"
wait
