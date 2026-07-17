#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Runs the generated .action runtime live acceptance cases as a reproducible matrix.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_ROOT="${1:-${TMPDIR:-/tmp}/ibmw-generated-action-live-matrix}"
RUN_CASES="${RUN_CASES:-fibonacci gripper_command navigate_to_pose follow_path navigate_through_poses execute_trajectory move_group}"

mkdir -p "${WORK_ROOT}"

case_index=0
for action_case in ${RUN_CASES}; do
  case_index=$((case_index + 1))
  case_work_dir="${WORK_ROOT}/${case_index}_${action_case}"
  echo "=========================================="
  echo "Generated action live matrix case ${case_index}: ${action_case}"
  echo "Work dir: ${case_work_dir}"
  echo "=========================================="
  ACTION_CASE="${action_case}" \
    bash "${ROOT_DIR}/scripts/live_ros2_generated_action_acceptance.sh" "${case_work_dir}"
done

python3 - "${WORK_ROOT}" ${RUN_CASES} <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
cases = sys.argv[2:]
metadata = {
    "fibonacci": ("example_interfaces/action/Fibonacci", "/fibonacci"),
    "gripper_command": ("control_msgs/action/GripperCommand", "/gripper_command"),
    "navigate_to_pose": ("nav2_msgs/action/NavigateToPose", "/navigate_to_pose"),
    "follow_path": ("nav2_msgs/action/FollowPath", "/follow_path"),
    "navigate_through_poses": ("nav2_msgs/action/NavigateThroughPoses", "/navigate_through_poses"),
    "execute_trajectory": ("moveit_msgs/action/ExecuteTrajectory", "/execute_trajectory"),
    "move_group": ("moveit_msgs/action/MoveGroup", "/move_action"),
}
summary_cases = []
for index, action_case in enumerate(cases, start=1):
    action_type, action_name = metadata.get(action_case, (None, None))
    summary_cases.append(
        {
            "case": action_case,
            "action_type": action_type,
            "action_name": action_name,
            "work_dir": str(work / f"{index}_{action_case}"),
            "runtime_template_pass": True,
            "user_hook_binding_contract": True,
            "status": "pass",
        }
    )
summary = {
    "schema_version": 1,
    "artifact": "action_runtime_matrix_summary",
    "lane": "action",
    "uses_rcl_in_ibmw_runtime": False,
    "uses_ros2_cli_as_import_prerequisite": False,
    "generic_business_semantics_generated": False,
    "cases": summary_cases,
    "totals": {"selected": len(summary_cases), "pass": len(summary_cases)},
}
path = work / "action_runtime_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] action runtime matrix summary: {path}")
PY

echo "[PASS] Generated action live matrix completed: ${RUN_CASES}"
