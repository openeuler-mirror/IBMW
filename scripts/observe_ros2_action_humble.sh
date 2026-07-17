#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# observe_ros2_action_humble.sh — read-only ROS 2 Humble action wire-profile
# observation helper.
#
# This script records facts only:
#   - no source edits
#   - no builds
#   - no process creation beyond read-only ros2 CLI queries
#   - no process killing
#   - no claim of IBMW support or integration
#   - optional Phase 1B DDS/FastDDS commands are printed only, never executed
#
# Default focus:
#   action name: /fibonacci
#   action type: example_interfaces/action/Fibonacci
#   RMW:         rmw_fastrtps_cpp
#
# Usage:
#   bash scripts/observe_ros2_action_humble.sh [--action-name NAME] [--action-type TYPE]
#   bash scripts/observe_ros2_action_humble.sh --help

set -euo pipefail

SCRIPT_NAME="observe_ros2_action_humble.sh"
DEFAULT_ACTION_NAME="/fibonacci"
DEFAULT_ACTION_TYPE="example_interfaces/action/Fibonacci"
DEFAULT_RMW_IMPLEMENTATION="rmw_fastrtps_cpp"

ACTION_NAME="$DEFAULT_ACTION_NAME"
ACTION_TYPE="$DEFAULT_ACTION_TYPE"

usage() {
  cat <<'EOF'
Usage:
  bash scripts/observe_ros2_action_humble.sh [--action-name NAME] [--action-type TYPE]
  bash scripts/observe_ros2_action_humble.sh --help

Read-only observation helper for ROS 2 Humble + FastDDS action wire-profile
facts. It prints commands and runs ROS 2 CLI queries when available.

Defaults:
  --action-name  /fibonacci
  --action-type  example_interfaces/action/Fibonacci

Behavior:
  - validates ROS 2 Humble is sourced
  - requires RMW_IMPLEMENTATION=rmw_fastrtps_cpp (defaults to that value if unset)
  - prints ros2 doctor/daemon status basics when available
  - prints action, interface, topic, and service observations
  - degrades gracefully when the action server or sample packages are absent
  - records facts only; it does not claim IBMW support
EOF
}

log() {
  printf '[%s] %s\n' "$SCRIPT_NAME" "$*"
}

warn() {
  printf '[%s] WARN: %s\n' "$SCRIPT_NAME" "$*" >&2
}

fail() {
  printf '[%s] FAIL: %s\n' "$SCRIPT_NAME" "$*" >&2
  exit 1
}

print_cmd() {
  printf '$ '
  printf '%q ' "$@"
  printf '\n'
}

run_cmd_optional() {
  local label="$1"
  shift
  print_cmd "$@"
  if "$@"; then
    return 0
  fi
  warn "$label command exited non-zero; continuing"
  return 0
}

has_command() {
  command -v "$1" >/dev/null 2>&1
}

trim_slash() {
  local value="$1"
  while [[ "$value" != "/" && "$value" == */ ]]; do
    value="${value%/}"
  done
  printf '%s' "$value"
}

action_base_name() {
  local action_name
  action_name="$(trim_slash "$1")"
  printf '%s' "$action_name"
}

show_tool_availability() {
  log "Phase 1B tool availability (read-only observation helpers)"

  if has_command ros2; then
    log "available: $(command -v ros2)"
  else
    warn "missing: ros2"
  fi

  if has_command fastdds; then
    log "available: $(command -v fastdds)"
  else
    warn "fastdds CLI not found on PATH"
  fi

  if has_command tcpdump; then
    log "available: $(command -v tcpdump)"
  else
    warn "tcpdump not found on PATH"
  fi

  if has_command tshark; then
    log "available: $(command -v tshark)"
  else
    warn "tshark not found on PATH (non-blocking; do not depend on it)"
  fi
}

interface_exists() {
  local interface_spec="$1"
  if ros2 interface show "$interface_spec" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

command_output_has_exact_line() {
  local expected_line="$1"
  shift

  local line
  while IFS= read -r line; do
    if [[ "$line" == "$expected_line" ]]; then
      return 0
    fi
  done < <("$@" 2>/dev/null)

  return 1
}

run_interface_show() {
  local label="$1"
  local interface_spec="$2"
  if interface_exists "$interface_spec"; then
    log "$label"
    run_cmd_optional "$label" ros2 interface show "$interface_spec"
  else
    warn "$label unavailable: $interface_spec"
  fi
}

check_action_present() {
  local action_name="$1"
  if ! ros2 action list >/dev/null 2>&1; then
    warn "ros2 action list is unavailable right now; action graph checks will be limited"
    return 1
  fi

  if command_output_has_exact_line "$action_name" ros2 action list; then
    return 0
  fi
  return 1
}

show_action_info_if_present() {
  local action_name="$1"
  if check_action_present "$action_name"; then
    log "action exists: $action_name"
    run_cmd_optional "ros2 action info" ros2 action info "$action_name"
  else
    warn "action not listed in the current graph: $action_name"
    warn "continuing with interface and topic/service observations only"
  fi
}

show_inferred_topics() {
  local action_name="$1"
  local base_name
  base_name="$(action_base_name "$action_name")"

  local feedback_topic="${base_name}/_action/feedback"
  local status_topic="${base_name}/_action/status"

  for topic_name in "$feedback_topic" "$status_topic"; do
    if command_output_has_exact_line "$topic_name" ros2 topic list --include-hidden-topics; then
      log "topic present: $topic_name"
      run_cmd_optional "ros2 topic info -v" ros2 topic info -v "$topic_name"
    else
      warn "topic not present yet: $topic_name"
    fi
  done
}

show_inferred_services() {
  local action_name="$1"
  local base_name
  base_name="$(action_base_name "$action_name")"

  local service_name
  for service_name in \
    "${base_name}/_action/send_goal" \
    "${base_name}/_action/get_result" \
    "${base_name}/_action/cancel_goal"; do
    if command_output_has_exact_line "$service_name" ros2 service list --include-hidden-services; then
      log "service present: $service_name"
      run_cmd_optional "ros2 service type" ros2 service type "$service_name"
    else
      warn "service not present yet: $service_name"
    fi
  done
}

show_lists() {
  log "topic list (including hidden topics)"
  run_cmd_optional "ros2 topic list" ros2 topic list --include-hidden-topics

  log "topic list with types (including hidden topics)"
  run_cmd_optional "ros2 topic list -t" ros2 topic list -t --include-hidden-topics

  log "service list (including hidden services)"
  run_cmd_optional "ros2 service list" ros2 service list --include-hidden-services

  log "service list with types (including hidden services)"
  run_cmd_optional "ros2 service list -t" ros2 service list -t --include-hidden-services
}

show_phase1b_guidance() {
  local action_name="$1"
  local action_type="$2"
  local base_name
  base_name="$(action_base_name "$action_name")"

  local feedback_topic="${base_name}/_action/feedback"
  local status_topic="${base_name}/_action/status"
  local send_goal_service="${base_name}/_action/send_goal"
  local get_result_service="${base_name}/_action/get_result"
  local cancel_goal_service="${base_name}/_action/cancel_goal"

  log "Phase 1B manual DDS/FastDDS observation guidance"
  cat <<EOF
Observed here means ROS 2 CLI facts only. Everything below is guidance for
additional Phase 1B evidence capture and should be treated as manual follow-up,
not as proof produced by this script.

Observed logical endpoints for action ${action_name}:
  topics:
    - ${feedback_topic}
    - ${status_topic}
  services:
    - ${send_goal_service}
    - ${get_result_service}
    - ${cancel_goal_service}

Inferred rmw_fastrtps / FastDDS expectations to verify manually:
  - The two logical topics above should map to DDS topics whose exact mangled
    names and DDS type names are implementation details until directly observed.
  - The three logical services above likely decompose into request/reply DDS
    entities in the same family as ROS 2 native service transport (commonly
    discussed as rq/rr pairs), but this helper does not claim the exact FastDDS
    topic strings, type names, or correlation fields.
  - Request/reply identity must be verified from FastDDS discovery output or
    packet capture before any DDS-only runtime design is authorized.

Optional manual commands for deeper evidence capture (printed only):
EOF

  if has_command fastdds; then
    print_cmd fastdds discovery --help
    print_cmd fastdds --help
  else
    warn "fastdds CLI unavailable; skip FastDDS discovery commands"
  fi

  if has_command tcpdump; then
    print_cmd tcpdump -i any -nn -s 0 -c 200 udp and portrange 7400-7500
  else
    warn "tcpdump unavailable; skip packet-capture command"
  fi

  print_cmd ros2 topic info -v "$feedback_topic"
  print_cmd ros2 topic info -v "$status_topic"
  print_cmd ros2 service list -t --include-hidden-services
  print_cmd ros2 action send_goal "$action_name" "$action_type" '{order: 20}' --feedback
  print_cmd ros2 service call "$cancel_goal_service" action_msgs/srv/CancelGoal '{goal_info: {goal_id: {uuid: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}, stamp: {sec: 0, nanosec: 0}}}'

  cat <<EOF

Cancel-goal note:
  - The cancel command above is intentionally printed only. A zero UUID + zero
    timestamp form is a conservative manual probe for "cancel all goals" in a
    controlled native test, but the resulting behavior still needs to be
    observed and recorded rather than assumed.

Packet/discovery note:
  - If tcpdump is available, capture RTPS discovery/user traffic first and keep
    claims limited to what the capture actually shows.
  - If fastdds tooling is available, use it to inspect discovered participants,
    readers, and writers; record exact DDS names/type names only when the tool
    output makes them explicit.
  - Do not depend on tshark being installed.
EOF
}

main() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --help|-h)
        usage
        exit 0
        ;;
      --action-name)
        if [[ $# -lt 2 ]]; then
          usage >&2
          exit 64
        fi
        ACTION_NAME="$2"
        shift 2
        ;;
      --action-type)
        if [[ $# -lt 2 ]]; then
          usage >&2
          exit 64
        fi
        ACTION_TYPE="$2"
        shift 2
        ;;
      *)
        warn "unknown argument: $1"
        usage >&2
        exit 64
        ;;
    esac
  done

  if ! has_command ros2; then
    fail "ros2 CLI not found on PATH"
  fi

  if [[ "${ROS_VERSION:-}" != "2" ]]; then
    fail "ROS 2 environment not detected (ROS_VERSION=${ROS_VERSION:-unset})"
  fi

  if [[ "${ROS_DISTRO:-}" != "humble" ]]; then
    fail "ROS 2 Humble is required (ROS_DISTRO=${ROS_DISTRO:-unset})"
  fi

  export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-$DEFAULT_RMW_IMPLEMENTATION}"
  if [[ "$RMW_IMPLEMENTATION" != "$DEFAULT_RMW_IMPLEMENTATION" ]]; then
    fail "RMW_IMPLEMENTATION must be ${DEFAULT_RMW_IMPLEMENTATION} (got: ${RMW_IMPLEMENTATION})"
  fi

  log "read-only ROS 2 Humble action observation helper"
  log "facts only; no source edits, no builds, no node startup, no process killing"
  log "ROS_DISTRO=${ROS_DISTRO}"
  log "RMW_IMPLEMENTATION=${RMW_IMPLEMENTATION}"
  log "action-name=${ACTION_NAME}"
  log "action-type=${ACTION_TYPE}"

  show_tool_availability

  if has_command ros2 && ros2 doctor --help >/dev/null 2>&1; then
    log "ros2 doctor"
    run_cmd_optional "ros2 doctor" ros2 doctor --report
  else
    warn "ros2 doctor is unavailable in this environment"
  fi

  if has_command ros2 && ros2 daemon --help >/dev/null 2>&1; then
    log "ros2 daemon status"
    run_cmd_optional "ros2 daemon status" ros2 daemon status
  else
    warn "ros2 daemon status is unavailable in this environment"
  fi

  log "action list"
  run_cmd_optional "ros2 action list" ros2 action list

  log "action list with types"
  run_cmd_optional "ros2 action list -t" ros2 action list -t

  log "node list"
  run_cmd_optional "ros2 node list" ros2 node list

  show_action_info_if_present "$ACTION_NAME"

  log "interface show: action type"
  run_interface_show "action type" "$ACTION_TYPE"

  log "interface show: action_msgs/service/action primitives"
  run_interface_show "action_msgs CancelGoal" "action_msgs/srv/CancelGoal"
  run_interface_show "action_msgs GoalStatusArray" "action_msgs/msg/GoalStatusArray"
  run_interface_show "action_msgs GoalStatus" "action_msgs/msg/GoalStatus"
  run_interface_show "action_msgs GoalInfo" "action_msgs/msg/GoalInfo"
  run_interface_show "unique_identifier_msgs UUID" "unique_identifier_msgs/msg/UUID"
  run_interface_show "builtin_interfaces Time" "builtin_interfaces/msg/Time"

  show_lists
  show_inferred_topics "$ACTION_NAME"
  show_inferred_services "$ACTION_NAME"
  show_phase1b_guidance "$ACTION_NAME" "$ACTION_TYPE"

  log "done"
}

main "$@"
