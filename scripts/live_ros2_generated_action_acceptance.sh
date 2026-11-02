#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Live ROS2 peer acceptance for generated .action runtime headers.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${1:-${TMPDIR:-/tmp}/ibmw-generated-action-live-acceptance}"
ROS2_PREFIX="${ROS2_PREFIX:-/opt/ros/humble}"
ROS2_SETUP="${ROS2_SETUP:-${ROS2_PREFIX}/setup.bash}"
ACTION_CASE="${ACTION_CASE:-fibonacci}"
ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"
GOAL_ORDER="${GOAL_ORDER:-8}"
GRIPPER_POSITION="${GRIPPER_POSITION:-0.42}"
GRIPPER_EFFORT="${GRIPPER_EFFORT:-3.0}"
CANCEL_DELAY_MS="${CANCEL_DELAY_MS:-10}"

case "${ACTION_CASE}" in
  fibonacci)
    PACKAGE_NAME="${PACKAGE_NAME:-example_interfaces}"
    ROS2_TYPE="${ROS2_TYPE:-action/Fibonacci}"
    ACTION_NAME="${ACTION_NAME:-/fibonacci}"
    CLIENT_BUILD_TARGET=ibmw_fibonacci_action_ros2_client
    CLIENT_TARGET_NAME=ibmw_fibonacci_action_ros2_client
    SERVER_TARGET_NAME=generated_fibonacci_success_server
    VALIDATION_ACTION=fibonacci
    ;;
  gripper_command)
    PACKAGE_NAME="${PACKAGE_NAME:-control_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/GripperCommand}"
    ACTION_NAME="${ACTION_NAME:-/gripper_command}"
    CLIENT_BUILD_TARGET=ibmw_gripper_command_action_ros2_client
    CLIENT_TARGET_NAME=ibmw_gripper_command_action_ros2_client
    SERVER_TARGET_NAME=generated_gripper_command_success_server
    VALIDATION_ACTION=gripper_command
    ;;
  navigate_to_pose)
    PACKAGE_NAME="${PACKAGE_NAME:-nav2_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/NavigateToPose}"
    ACTION_NAME="${ACTION_NAME:-/navigate_to_pose}"
    CLIENT_BUILD_TARGET=
    CLIENT_TARGET_NAME=nav2_navigate_to_pose_rclpy_client
    SERVER_TARGET_NAME=generated_navigate_to_pose_success_server
    VALIDATION_ACTION=navigate_to_pose
    ;;
  follow_path)
    PACKAGE_NAME="${PACKAGE_NAME:-nav2_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/FollowPath}"
    ACTION_NAME="${ACTION_NAME:-/follow_path}"
    CLIENT_BUILD_TARGET=
    CLIENT_TARGET_NAME=nav2_follow_path_rclpy_client
    SERVER_TARGET_NAME=generated_follow_path_success_server
    VALIDATION_ACTION=follow_path
    ;;
  navigate_through_poses)
    PACKAGE_NAME="${PACKAGE_NAME:-nav2_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/NavigateThroughPoses}"
    ACTION_NAME="${ACTION_NAME:-/navigate_through_poses}"
    CLIENT_BUILD_TARGET=
    CLIENT_TARGET_NAME=nav2_navigate_through_poses_rclpy_client
    SERVER_TARGET_NAME=generated_navigate_through_poses_success_server
    VALIDATION_ACTION=navigate_through_poses
    ;;
  execute_trajectory)
    PACKAGE_NAME="${PACKAGE_NAME:-moveit_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/ExecuteTrajectory}"
    ACTION_NAME="${ACTION_NAME:-/execute_trajectory}"
    CLIENT_BUILD_TARGET=
    CLIENT_TARGET_NAME=moveit_execute_trajectory_rclpy_client
    SERVER_TARGET_NAME=generated_execute_trajectory_success_server
    VALIDATION_ACTION=execute_trajectory
    ;;
  move_group)
    PACKAGE_NAME="${PACKAGE_NAME:-moveit_msgs}"
    ROS2_TYPE="${ROS2_TYPE:-action/MoveGroup}"
    ACTION_NAME="${ACTION_NAME:-/move_action}"
    CLIENT_BUILD_TARGET=
    CLIENT_TARGET_NAME=moveit_move_group_rclpy_client
    SERVER_TARGET_NAME=generated_move_group_success_server
    VALIDATION_ACTION=move_group
    ;;
  *)
    echo "[FAIL] unsupported ACTION_CASE=${ACTION_CASE}; expected fibonacci, gripper_command, navigate_to_pose, follow_path, navigate_through_poses, execute_trajectory, or move_group" >&2
    exit 1
    ;;
esac
SERVER_READY_MARKER="${SERVER_READY_MARKER:-DDS-only action server started for ${ACTION_NAME}}"

fail() {
  echo "[FAIL] $*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || fail "required file not found: $1"
}

source_ros2() {
  require_file "${ROS2_SETUP}"
  set +u
  # shellcheck disable=SC1090
  source "${ROS2_SETUP}"
  set -u
  export ROS_DOMAIN_ID
  export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
  export LD_LIBRARY_PATH="${ROS2_PREFIX}/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
  export PYTHONPATH="${ROS2_PREFIX}/lib/python3.10/site-packages:${ROS2_PREFIX}/local/lib/python3.10/dist-packages:${PYTHONPATH:-}"
  export CMAKE_PREFIX_PATH="${ROS2_PREFIX}:${CMAKE_PREFIX_PATH:-}"
  export AMENT_PREFIX_PATH="${ROS2_PREFIX}:${AMENT_PREFIX_PATH:-}"
}

write_success_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <example_interfaces/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct FibonacciHook {
  using GoalInput = int32_t;
  using FeedbackPayload = std::vector<int32_t>;
  using ResultPayload = std::vector<int32_t>;

  struct ExecState {
    int32_t order = 0;
    std::vector<int32_t> sequence;
  };

  static bool ValidateGoal(const GoalInput& order) { return order > 0 && order <= 47; }

  static ExecState InitState(const GoalInput& order) {
    ExecState state;
    state.order = order;
    state.sequence = {0};
    if (order > 1) {
      state.sequence.push_back(1);
    }
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    if (static_cast<int32_t>(state.sequence.size()) >= state.order) {
      return ibmw::action::TickOutcome::kSucceeded;
    }
    const auto n = state.sequence.size();
    state.sequence.push_back(state.sequence[n - 1] + state.sequence[n - 2]);
    return ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) { return state.sequence; }
  static ResultPayload BuildResult(const ExecState& state) { return state.sequence; }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::example_interfaces::FibonacciActionTraits,
    ::generated_probe::FibonacciHook> {
  using Traits = ::ibmw::ros2_interfaces::example_interfaces::FibonacciActionTraits;
  using Hook = ::generated_probe::FibonacciHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return request.goal.order;
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.sequence = result;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.sequence = feedback;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::example_interfaces::FibonacciActionTraits,
    ::generated_probe::FibonacciHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/fibonacci";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::example_interfaces::FibonacciActionServerRuntime<generated_probe::FibonacciHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_gripper_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <control_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct GripperPayload {
  double position = 0.0;
  double effort = 0.0;
  bool stalled = false;
  bool reached_goal = false;
};

struct GripperCommandHook {
  using GoalInput = GripperPayload;
  using FeedbackPayload = GripperPayload;
  using ResultPayload = GripperPayload;

  struct ExecState {
    double target_position = 0.0;
    double max_effort = 0.0;
    double current_position = 0.0;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput& goal) {
    return std::isfinite(goal.position) && std::isfinite(goal.effort) &&
           goal.position >= 0.0 && goal.position <= 1.0 && goal.effort >= 0.0;
  }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.target_position = goal.position;
    state.max_effort = goal.effort;
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    const double progress = std::min(1.0, static_cast<double>(state.step) / 5.0);
    state.current_position = state.target_position * progress;
    return state.step >= 5 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    return GripperPayload{.position = state.current_position,
                          .effort = state.max_effort,
                          .stalled = false,
                          .reached_goal = state.step >= 5};
  }

  static ResultPayload BuildResult(const ExecState& state) {
    return GripperPayload{.position = state.target_position,
                          .effort = state.max_effort,
                          .stalled = false,
                          .reached_goal = true};
  }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::control_msgs::GripperCommandActionTraits,
    ::generated_probe::GripperCommandHook> {
  using Traits = ::ibmw::ros2_interfaces::control_msgs::GripperCommandActionTraits;
  using Hook = ::generated_probe::GripperCommandHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.position = request.goal.command.position,
                           .effort = request.goal.command.max_effort,
                           .stalled = false,
                           .reached_goal = false};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.position = result.position;
    response.result.effort = result.effort;
    response.result.stalled = result.stalled;
    response.result.reached_goal = result.reached_goal;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.position = feedback.position;
    message.feedback.effort = feedback.effort;
    message.feedback.stalled = feedback.stalled;
    message.feedback.reached_goal = feedback.reached_goal;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::control_msgs::GripperCommandActionTraits,
    ::generated_probe::GripperCommandHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/gripper_command";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::control_msgs::GripperCommandActionServerRuntime<generated_probe::GripperCommandHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_nav2_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <variant>

#include <nav2_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct NavigateGoalInput {
  geometry_msgs::msg::PoseStamped pose;
  std::string behavior_tree;
};

struct NavigateFeedbackPayload {
  geometry_msgs::msg::PoseStamped current_pose;
  float distance_remaining = 0.0F;
  int step = 0;
};

struct NavigateToPoseHook {
  using GoalInput = NavigateGoalInput;
  using FeedbackPayload = NavigateFeedbackPayload;
  using ResultPayload = std::monostate;

  struct ExecState {
    geometry_msgs::msg::PoseStamped target_pose;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput&) { return true; }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.target_pose = goal.pose;
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    return state.step >= 3 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    FeedbackPayload feedback;
    feedback.current_pose = state.target_pose;
    feedback.current_pose.pose.position.x = state.target_pose.pose.position.x *
                                            static_cast<double>(state.step) / 3.0;
    feedback.distance_remaining = static_cast<float>(3 - state.step);
    feedback.step = state.step;
    return feedback;
  }

  static ResultPayload BuildResult(const ExecState&) { return {}; }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::nav2_msgs::NavigateToPoseActionTraits,
    ::generated_probe::NavigateToPoseHook> {
  using Traits = ::ibmw::ros2_interfaces::nav2_msgs::NavigateToPoseActionTraits;
  using Hook = ::generated_probe::NavigateToPoseHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.pose = request.goal.pose, .behavior_tree = request.goal.behavior_tree};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload&) {
    Traits::GetResultResponse response;
    response.status = status;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.current_pose = feedback.current_pose;
    message.feedback.distance_remaining = feedback.distance_remaining;
    message.feedback.number_of_recoveries = static_cast<int16_t>(feedback.step);
    message.feedback.navigation_time.sec = feedback.step;
    message.feedback.estimated_time_remaining.sec = 3 - feedback.step;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::nav2_msgs::NavigateToPoseActionTraits,
    ::generated_probe::NavigateToPoseHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/navigate_to_pose";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::nav2_msgs::NavigateToPoseActionServerRuntime<generated_probe::NavigateToPoseHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_nav2_client() {
  local output=$1
  cat >"${output}" <<'PY'
#!/usr/bin/env python3
import argparse
import sys
import time

import rclpy
from rclpy.action import ActionClient
from action_msgs.msg import GoalStatus
from nav2_msgs.action import NavigateToPose


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["success", "cancel"], default="success")
    parser.add_argument("--action-name", default="/navigate_to_pose")
    parser.add_argument("--server-wait-seconds", type=float, default=10.0)
    parser.add_argument("--result-wait-seconds", type=float, default=20.0)
    parser.add_argument("--cancel-delay-ms", type=int, default=10)
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("ibmw_nav2_navigate_to_pose_rclpy_client")
    client = ActionClient(node, NavigateToPose, args.action_name)
    feedback = []
    try:
        print(f"[INFO] action_name={args.action_name} mode={args.mode}")
        if not client.wait_for_server(timeout_sec=args.server_wait_seconds):
            print("[FAIL] action server not available within timeout", file=sys.stderr)
            return 1
        goal = NavigateToPose.Goal()
        goal.pose.header.frame_id = "map"
        goal.pose.pose.position.x = 1.5
        goal.pose.pose.orientation.w = 1.0

        def feedback_cb(msg):
            feedback.append(msg.feedback.distance_remaining)
            print(f"[INFO] Feedback distance_remaining={msg.feedback.distance_remaining} recoveries={msg.feedback.number_of_recoveries}")

        send_future = client.send_goal_async(goal, feedback_callback=feedback_cb)
        rclpy.spin_until_future_complete(node, send_future, timeout_sec=args.result_wait_seconds)
        goal_handle = send_future.result()
        if goal_handle is None or not goal_handle.accepted:
            print("[FAIL] goal was rejected by server", file=sys.stderr)
            return 1
        print("[INFO] Goal accepted")

        if args.mode == "cancel":
            time.sleep(args.cancel_delay_ms / 1000.0)
            cancel_future = goal_handle.cancel_goal_async()
            rclpy.spin_until_future_complete(node, cancel_future, timeout_sec=args.result_wait_seconds)
            cancel_response = cancel_future.result()
            goals_canceling = len(cancel_response.goals_canceling) if cancel_response else 0
            print(f"[INFO] Cancel return_code=0 goals_canceling={goals_canceling}")
            if goals_canceling != 1:
                print("[FAIL] cancel request was not accepted", file=sys.stderr)
                return 1

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(node, result_future, timeout_sec=args.result_wait_seconds)
        result = result_future.result()
        if result is None:
            print("[FAIL] timed out waiting for terminal result", file=sys.stderr)
            return 1
        if args.mode == "success":
            if result.status != GoalStatus.STATUS_SUCCEEDED:
                print(f"[FAIL] expected SUCCEEDED result code, got {result.status}", file=sys.stderr)
                return 1
            if not feedback:
                print("[FAIL] expected at least one feedback callback", file=sys.stderr)
                return 1
            print("[PASS] Success case matched deterministic NavigateToPose result")
            return 0
        if result.status != GoalStatus.STATUS_CANCELED:
            print(f"[FAIL] expected CANCELED result code, got {result.status}", file=sys.stderr)
            return 1
        print("[PASS] Cancel case reached terminal CANCELED status")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
PY
  chmod +x "${output}"
}

write_follow_path_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <variant>

#include <nav2_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct FollowPathGoalInput {
  nav_msgs::msg::Path path;
  std::string controller_id;
  std::string goal_checker_id;
};

struct FollowPathFeedbackPayload {
  float distance_to_goal = 0.0F;
  float speed = 0.0F;
  int step = 0;
};

struct FollowPathHook {
  using GoalInput = FollowPathGoalInput;
  using FeedbackPayload = FollowPathFeedbackPayload;
  using ResultPayload = std::monostate;

  struct ExecState {
    std::size_t pose_count = 0;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput& goal) { return !goal.path.poses.empty(); }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.pose_count = goal.path.poses.size();
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    return state.step >= 3 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    FeedbackPayload feedback;
    feedback.step = state.step;
    feedback.distance_to_goal = static_cast<float>(3 - state.step);
    feedback.speed = static_cast<float>(state.pose_count) * 0.25F + static_cast<float>(state.step);
    return feedback;
  }

  static ResultPayload BuildResult(const ExecState&) { return {}; }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::nav2_msgs::FollowPathActionTraits,
    ::generated_probe::FollowPathHook> {
  using Traits = ::ibmw::ros2_interfaces::nav2_msgs::FollowPathActionTraits;
  using Hook = ::generated_probe::FollowPathHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.path = request.goal.path,
                           .controller_id = request.goal.controller_id,
                           .goal_checker_id = request.goal.goal_checker_id};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload&) {
    Traits::GetResultResponse response;
    response.status = status;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.distance_to_goal = feedback.distance_to_goal;
    message.feedback.speed = feedback.speed;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::nav2_msgs::FollowPathActionTraits,
    ::generated_probe::FollowPathHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/follow_path";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::nav2_msgs::FollowPathActionServerRuntime<generated_probe::FollowPathHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_follow_path_client() {
  local output=$1
  cat >"${output}" <<'PY'
#!/usr/bin/env python3
import argparse
import sys
import time

import rclpy
from rclpy.action import ActionClient
from action_msgs.msg import GoalStatus
from geometry_msgs.msg import PoseStamped
from nav2_msgs.action import FollowPath


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["success", "cancel"], default="success")
    parser.add_argument("--action-name", default="/follow_path")
    parser.add_argument("--server-wait-seconds", type=float, default=10.0)
    parser.add_argument("--result-wait-seconds", type=float, default=20.0)
    parser.add_argument("--cancel-delay-ms", type=int, default=10)
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("ibmw_nav2_follow_path_rclpy_client")
    client = ActionClient(node, FollowPath, args.action_name)
    feedback = []
    try:
        print(f"[INFO] action_name={args.action_name} mode={args.mode}")
        if not client.wait_for_server(timeout_sec=args.server_wait_seconds):
            print("[FAIL] action server not available within timeout", file=sys.stderr)
            return 1
        goal = FollowPath.Goal()
        goal.path.header.frame_id = "map"
        for index in range(3):
            pose = PoseStamped()
            pose.header.frame_id = "map"
            pose.pose.position.x = float(index)
            pose.pose.orientation.w = 1.0
            goal.path.poses.append(pose)
        goal.controller_id = "ibmw_probe_controller"
        goal.goal_checker_id = "ibmw_probe_goal_checker"

        def feedback_cb(msg):
            feedback.append(msg.feedback.distance_to_goal)
            print(f"[INFO] Feedback distance_to_goal={msg.feedback.distance_to_goal} speed={msg.feedback.speed}")

        send_future = client.send_goal_async(goal, feedback_callback=feedback_cb)
        rclpy.spin_until_future_complete(node, send_future, timeout_sec=args.result_wait_seconds)
        goal_handle = send_future.result()
        if goal_handle is None or not goal_handle.accepted:
            print("[FAIL] goal was rejected by server", file=sys.stderr)
            return 1
        print("[INFO] Goal accepted")

        if args.mode == "cancel":
            time.sleep(args.cancel_delay_ms / 1000.0)
            cancel_future = goal_handle.cancel_goal_async()
            rclpy.spin_until_future_complete(node, cancel_future, timeout_sec=args.result_wait_seconds)
            cancel_response = cancel_future.result()
            goals_canceling = len(cancel_response.goals_canceling) if cancel_response else 0
            print(f"[INFO] Cancel return_code=0 goals_canceling={goals_canceling}")
            if goals_canceling != 1:
                print("[FAIL] cancel request was not accepted", file=sys.stderr)
                return 1

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(node, result_future, timeout_sec=args.result_wait_seconds)
        result = result_future.result()
        if result is None:
            print("[FAIL] timed out waiting for terminal result", file=sys.stderr)
            return 1
        if args.mode == "success":
            if result.status != GoalStatus.STATUS_SUCCEEDED:
                print(f"[FAIL] expected SUCCEEDED result code, got {result.status}", file=sys.stderr)
                return 1
            if not feedback:
                print("[FAIL] expected at least one feedback callback", file=sys.stderr)
                return 1
            print("[PASS] Success case matched deterministic FollowPath result")
            return 0
        if result.status != GoalStatus.STATUS_CANCELED:
            print(f"[FAIL] expected CANCELED result code, got {result.status}", file=sys.stderr)
            return 1
        print("[PASS] Cancel case reached terminal CANCELED status")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
PY
  chmod +x "${output}"
}

write_navigate_through_poses_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nav2_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct NavigateThroughPosesGoalInput {
  std::vector<geometry_msgs::msg::PoseStamped> poses;
  std::string behavior_tree;
};

struct NavigateThroughPosesFeedbackPayload {
  geometry_msgs::msg::PoseStamped current_pose;
  float distance_remaining = 0.0F;
  int step = 0;
  int poses_remaining = 0;
};

struct NavigateThroughPosesHook {
  using GoalInput = NavigateThroughPosesGoalInput;
  using FeedbackPayload = NavigateThroughPosesFeedbackPayload;
  using ResultPayload = std::monostate;

  struct ExecState {
    std::vector<geometry_msgs::msg::PoseStamped> poses;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput& goal) { return !goal.poses.empty(); }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.poses = goal.poses;
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    return state.step >= 3 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    FeedbackPayload feedback;
    const std::size_t index = std::min<std::size_t>(
        state.poses.size() - 1U, static_cast<std::size_t>(std::max(0, state.step - 1)));
    feedback.current_pose = state.poses[index];
    feedback.distance_remaining = static_cast<float>(3 - state.step);
    feedback.step = state.step;
    feedback.poses_remaining = std::max(0, static_cast<int>(state.poses.size()) - state.step);
    return feedback;
  }

  static ResultPayload BuildResult(const ExecState&) { return {}; }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::nav2_msgs::NavigateThroughPosesActionTraits,
    ::generated_probe::NavigateThroughPosesHook> {
  using Traits = ::ibmw::ros2_interfaces::nav2_msgs::NavigateThroughPosesActionTraits;
  using Hook = ::generated_probe::NavigateThroughPosesHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.poses = request.goal.poses, .behavior_tree = request.goal.behavior_tree};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload&) {
    Traits::GetResultResponse response;
    response.status = status;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.current_pose = feedback.current_pose;
    message.feedback.distance_remaining = feedback.distance_remaining;
    message.feedback.number_of_recoveries = static_cast<int16_t>(feedback.step);
    message.feedback.number_of_poses_remaining = static_cast<int16_t>(feedback.poses_remaining);
    message.feedback.navigation_time.sec = feedback.step;
    message.feedback.estimated_time_remaining.sec = 3 - feedback.step;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::nav2_msgs::NavigateThroughPosesActionTraits,
    ::generated_probe::NavigateThroughPosesHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/navigate_through_poses";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::nav2_msgs::NavigateThroughPosesActionServerRuntime<generated_probe::NavigateThroughPosesHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_navigate_through_poses_client() {
  local output=$1
  cat >"${output}" <<'PY'
#!/usr/bin/env python3
import argparse
import sys
import time

import rclpy
from rclpy.action import ActionClient
from action_msgs.msg import GoalStatus
from geometry_msgs.msg import PoseStamped
from nav2_msgs.action import NavigateThroughPoses


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["success", "cancel"], default="success")
    parser.add_argument("--action-name", default="/navigate_through_poses")
    parser.add_argument("--server-wait-seconds", type=float, default=10.0)
    parser.add_argument("--result-wait-seconds", type=float, default=20.0)
    parser.add_argument("--cancel-delay-ms", type=int, default=10)
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("ibmw_nav2_navigate_through_poses_rclpy_client")
    client = ActionClient(node, NavigateThroughPoses, args.action_name)
    feedback = []
    try:
        print(f"[INFO] action_name={args.action_name} mode={args.mode}")
        if not client.wait_for_server(timeout_sec=args.server_wait_seconds):
            print("[FAIL] action server not available within timeout", file=sys.stderr)
            return 1
        goal = NavigateThroughPoses.Goal()
        for index in range(4):
            pose = PoseStamped()
            pose.header.frame_id = "map"
            pose.pose.position.x = float(index)
            pose.pose.position.y = float(index) * 0.5
            pose.pose.orientation.w = 1.0
            goal.poses.append(pose)

        def feedback_cb(msg):
            feedback.append(msg.feedback.number_of_poses_remaining)
            print(f"[INFO] Feedback distance_remaining={msg.feedback.distance_remaining} poses_remaining={msg.feedback.number_of_poses_remaining}")

        send_future = client.send_goal_async(goal, feedback_callback=feedback_cb)
        rclpy.spin_until_future_complete(node, send_future, timeout_sec=args.result_wait_seconds)
        goal_handle = send_future.result()
        if goal_handle is None or not goal_handle.accepted:
            print("[FAIL] goal was rejected by server", file=sys.stderr)
            return 1
        print("[INFO] Goal accepted")

        if args.mode == "cancel":
            time.sleep(args.cancel_delay_ms / 1000.0)
            cancel_future = goal_handle.cancel_goal_async()
            rclpy.spin_until_future_complete(node, cancel_future, timeout_sec=args.result_wait_seconds)
            cancel_response = cancel_future.result()
            goals_canceling = len(cancel_response.goals_canceling) if cancel_response else 0
            print(f"[INFO] Cancel return_code=0 goals_canceling={goals_canceling}")
            if goals_canceling != 1:
                print("[FAIL] cancel request was not accepted", file=sys.stderr)
                return 1

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(node, result_future, timeout_sec=args.result_wait_seconds)
        result = result_future.result()
        if result is None:
            print("[FAIL] timed out waiting for terminal result", file=sys.stderr)
            return 1
        if args.mode == "success":
            if result.status != GoalStatus.STATUS_SUCCEEDED:
                print(f"[FAIL] expected SUCCEEDED result code, got {result.status}", file=sys.stderr)
                return 1
            if not feedback:
                print("[FAIL] expected at least one feedback callback", file=sys.stderr)
                return 1
            print("[PASS] Success case matched deterministic NavigateThroughPoses result")
            return 0
        if result.status != GoalStatus.STATUS_CANCELED:
            print(f"[FAIL] expected CANCELED result code, got {result.status}", file=sys.stderr)
            return 1
        print("[PASS] Cancel case reached terminal CANCELED status")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
PY
  chmod +x "${output}"
}

write_execute_trajectory_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <variant>

#include <moveit_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct ExecuteTrajectoryGoalInput {
  moveit_msgs::msg::RobotTrajectory trajectory;
};

struct ExecuteTrajectoryFeedbackPayload {
  std::string state;
};

struct ExecuteTrajectoryResultPayload {
  moveit_msgs::msg::MoveItErrorCodes error_code;
};

struct ExecuteTrajectoryHook {
  using GoalInput = ExecuteTrajectoryGoalInput;
  using FeedbackPayload = ExecuteTrajectoryFeedbackPayload;
  using ResultPayload = ExecuteTrajectoryResultPayload;

  struct ExecState {
    std::size_t point_count = 0;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput&) { return true; }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.point_count = goal.trajectory.joint_trajectory.points.size();
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    return state.step >= 3 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    FeedbackPayload feedback;
    feedback.state = "executing_step_" + std::to_string(state.step) +
                     "_points_" + std::to_string(state.point_count);
    return feedback;
  }

  static ResultPayload BuildResult(const ExecState&) {
    ResultPayload result;
    result.error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
    return result;
  }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::moveit_msgs::ExecuteTrajectoryActionTraits,
    ::generated_probe::ExecuteTrajectoryHook> {
  using Traits = ::ibmw::ros2_interfaces::moveit_msgs::ExecuteTrajectoryActionTraits;
  using Hook = ::generated_probe::ExecuteTrajectoryHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.trajectory = request.goal.trajectory};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.error_code = result.error_code;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.state = feedback.state;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::moveit_msgs::ExecuteTrajectoryActionTraits,
    ::generated_probe::ExecuteTrajectoryHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/execute_trajectory";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::moveit_msgs::ExecuteTrajectoryActionServerRuntime<generated_probe::ExecuteTrajectoryHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_execute_trajectory_client() {
  local output=$1
  cat >"${output}" <<'PY'
#!/usr/bin/env python3
import argparse
import sys
import time

import rclpy
from rclpy.action import ActionClient
from action_msgs.msg import GoalStatus
from moveit_msgs.action import ExecuteTrajectory
from moveit_msgs.msg import MoveItErrorCodes
from trajectory_msgs.msg import JointTrajectoryPoint


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["success", "cancel"], default="success")
    parser.add_argument("--action-name", default="/execute_trajectory")
    parser.add_argument("--server-wait-seconds", type=float, default=10.0)
    parser.add_argument("--result-wait-seconds", type=float, default=20.0)
    parser.add_argument("--cancel-delay-ms", type=int, default=10)
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("ibmw_moveit_execute_trajectory_rclpy_client")
    client = ActionClient(node, ExecuteTrajectory, args.action_name)
    feedback = []
    try:
        print(f"[INFO] action_name={args.action_name} mode={args.mode}")
        if not client.wait_for_server(timeout_sec=args.server_wait_seconds):
            print("[FAIL] action server not available within timeout", file=sys.stderr)
            return 1
        goal = ExecuteTrajectory.Goal()
        goal.trajectory.joint_trajectory.header.frame_id = "base_link"
        goal.trajectory.joint_trajectory.joint_names = ["joint_1", "joint_2"]
        for index in range(2):
            point = JointTrajectoryPoint()
            point.positions = [float(index), float(index) + 0.5]
            point.velocities = [0.1, 0.2]
            point.time_from_start.sec = index + 1
            goal.trajectory.joint_trajectory.points.append(point)

        def feedback_cb(msg):
            feedback.append(msg.feedback.state)
            print(f"[INFO] Feedback state={msg.feedback.state}")

        send_future = client.send_goal_async(goal, feedback_callback=feedback_cb)
        rclpy.spin_until_future_complete(node, send_future, timeout_sec=args.result_wait_seconds)
        goal_handle = send_future.result()
        if goal_handle is None or not goal_handle.accepted:
            print("[FAIL] goal was rejected by server", file=sys.stderr)
            return 1
        print("[INFO] Goal accepted")

        if args.mode == "cancel":
            time.sleep(args.cancel_delay_ms / 1000.0)
            cancel_future = goal_handle.cancel_goal_async()
            rclpy.spin_until_future_complete(node, cancel_future, timeout_sec=args.result_wait_seconds)
            cancel_response = cancel_future.result()
            goals_canceling = len(cancel_response.goals_canceling) if cancel_response else 0
            print(f"[INFO] Cancel return_code=0 goals_canceling={goals_canceling}")
            if goals_canceling != 1:
                print("[FAIL] cancel request was not accepted", file=sys.stderr)
                return 1

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(node, result_future, timeout_sec=args.result_wait_seconds)
        result = result_future.result()
        if result is None:
            print("[FAIL] timed out waiting for terminal result", file=sys.stderr)
            return 1
        if args.mode == "success":
            if result.status != GoalStatus.STATUS_SUCCEEDED:
                print(f"[FAIL] expected SUCCEEDED result code, got {result.status}", file=sys.stderr)
                return 1
            if result.result.error_code.val != MoveItErrorCodes.SUCCESS:
                print(f"[FAIL] expected MoveIt SUCCESS error code, got {result.result.error_code.val}", file=sys.stderr)
                return 1
            if not feedback:
                print("[FAIL] expected at least one feedback callback", file=sys.stderr)
                return 1
            print("[PASS] Success case matched deterministic ExecuteTrajectory result")
            return 0
        if result.status != GoalStatus.STATUS_CANCELED:
            print(f"[FAIL] expected CANCELED result code, got {result.status}", file=sys.stderr)
            return 1
        print("[PASS] Cancel case reached terminal CANCELED status")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
PY
  chmod +x "${output}"
}

write_move_group_server_source() {
  local output=$1
  cat >"${output}" <<'CPP'
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <variant>

#include <moveit_msgs/ibmw_action_server_runtime.hpp>

namespace generated_probe {

struct MoveGroupGoalInput {
  moveit_msgs::msg::MotionPlanRequest request;
  moveit_msgs::msg::PlanningOptions planning_options;
};

struct MoveGroupFeedbackPayload {
  std::string state;
};

struct MoveGroupResultPayload {
  moveit_msgs::msg::MoveItErrorCodes error_code;
  double planning_time = 0.0;
};

struct MoveGroupHook {
  using GoalInput = MoveGroupGoalInput;
  using FeedbackPayload = MoveGroupFeedbackPayload;
  using ResultPayload = MoveGroupResultPayload;

  struct ExecState {
    std::string group_name;
    int step = 0;
  };

  static bool ValidateGoal(const GoalInput&) { return true; }

  static ExecState InitState(const GoalInput& goal) {
    ExecState state;
    state.group_name = goal.request.group_name;
    return state;
  }

  static ibmw::action::TickOutcome Tick(ExecState& state) {
    ++state.step;
    return state.step >= 3 ? ibmw::action::TickOutcome::kSucceeded
                           : ibmw::action::TickOutcome::kContinue;
  }

  static FeedbackPayload BuildFeedback(const ExecState& state) {
    FeedbackPayload feedback;
    feedback.state = "planning_step_" + std::to_string(state.step) +
                     "_group_" + (state.group_name.empty() ? "default" : state.group_name);
    return feedback;
  }

  static ResultPayload BuildResult(const ExecState& state) {
    ResultPayload result;
    result.error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
    result.planning_time = 0.01 * static_cast<double>(state.step);
    return result;
  }
};

}  // namespace generated_probe

namespace ibmw::action {

template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::moveit_msgs::MoveGroupActionTraits,
    ::generated_probe::MoveGroupHook> {
  using Traits = ::ibmw::ros2_interfaces::moveit_msgs::MoveGroupActionTraits;
  using Hook = ::generated_probe::MoveGroupHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest& request) {
    return Hook::GoalInput{.request = request.goal.request,
                           .planning_options = request.goal.planning_options};
  }

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload& result) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.error_code = result.error_code;
    response.result.planning_time = result.planning_time;
    return response;
  }

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload& feedback) {
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    message.feedback.state = feedback.state;
    return message;
  }
};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::moveit_msgs::MoveGroupActionTraits,
    ::generated_probe::MoveGroupHook> = true;

}  // namespace ibmw::action

namespace {
std::atomic_bool stop{false};

void HandleSignal(int) { stop.store(true); }

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  return domain && std::strlen(domain) ? static_cast<uint32_t>(std::stoul(domain)) : 0U;
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/move_action";
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  ibmw::ros2_interfaces::moveit_msgs::MoveGroupActionServerRuntime<generated_probe::MoveGroupHook>
      runtime(ParseActionName(argc, argv));
  if (!runtime.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return 0;
}
CPP
}

write_move_group_client() {
  local output=$1
  cat >"${output}" <<'PY'
#!/usr/bin/env python3
import argparse
import sys
import time

import rclpy
from rclpy.action import ActionClient
from action_msgs.msg import GoalStatus
from moveit_msgs.action import MoveGroup
from moveit_msgs.msg import MoveItErrorCodes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["success", "cancel"], default="success")
    parser.add_argument("--action-name", default="/move_action")
    parser.add_argument("--server-wait-seconds", type=float, default=10.0)
    parser.add_argument("--result-wait-seconds", type=float, default=20.0)
    parser.add_argument("--cancel-delay-ms", type=int, default=10)
    args = parser.parse_args()

    rclpy.init()
    node = rclpy.create_node("ibmw_moveit_move_group_rclpy_client")
    client = ActionClient(node, MoveGroup, args.action_name)
    feedback = []
    try:
        print(f"[INFO] action_name={args.action_name} mode={args.mode}")
        if not client.wait_for_server(timeout_sec=args.server_wait_seconds):
            print("[FAIL] action server not available within timeout", file=sys.stderr)
            return 1
        goal = MoveGroup.Goal()
        goal.request.group_name = "ibmw_probe_group"
        goal.request.num_planning_attempts = 1
        goal.request.allowed_planning_time = 0.25
        goal.planning_options.plan_only = True

        def feedback_cb(msg):
            feedback.append(msg.feedback.state)
            print(f"[INFO] Feedback state={msg.feedback.state}")

        send_future = client.send_goal_async(goal, feedback_callback=feedback_cb)
        rclpy.spin_until_future_complete(node, send_future, timeout_sec=args.result_wait_seconds)
        goal_handle = send_future.result()
        if goal_handle is None or not goal_handle.accepted:
            print("[FAIL] goal was rejected by server", file=sys.stderr)
            return 1
        print("[INFO] Goal accepted")

        if args.mode == "cancel":
            time.sleep(args.cancel_delay_ms / 1000.0)
            cancel_future = goal_handle.cancel_goal_async()
            rclpy.spin_until_future_complete(node, cancel_future, timeout_sec=args.result_wait_seconds)
            cancel_response = cancel_future.result()
            goals_canceling = len(cancel_response.goals_canceling) if cancel_response else 0
            print(f"[INFO] Cancel return_code=0 goals_canceling={goals_canceling}")
            if goals_canceling != 1:
                print("[FAIL] cancel request was not accepted", file=sys.stderr)
                return 1

        result_future = goal_handle.get_result_async()
        rclpy.spin_until_future_complete(node, result_future, timeout_sec=args.result_wait_seconds)
        result = result_future.result()
        if result is None:
            print("[FAIL] timed out waiting for terminal result", file=sys.stderr)
            return 1
        if args.mode == "success":
            if result.status != GoalStatus.STATUS_SUCCEEDED:
                print(f"[FAIL] expected SUCCEEDED result code, got {result.status}", file=sys.stderr)
                return 1
            if result.result.error_code.val != MoveItErrorCodes.SUCCESS:
                print(f"[FAIL] expected MoveIt SUCCESS error code, got {result.result.error_code.val}", file=sys.stderr)
                return 1
            if not feedback:
                print("[FAIL] expected at least one feedback callback", file=sys.stderr)
                return 1
            print("[PASS] Success case matched deterministic MoveGroup result")
            return 0
        if result.status != GoalStatus.STATUS_CANCELED:
            print(f"[FAIL] expected CANCELED result code, got {result.status}", file=sys.stderr)
            return 1
        print("[PASS] Cancel case reached terminal CANCELED status")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
PY
  chmod +x "${output}"
}

write_server_cmake() {
  local output=$1
  local import_dir=$2
  cat >"${output}" <<CMAKE
cmake_minimum_required(VERSION 3.16)
project(generated_action_success_server_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(action_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(control_msgs REQUIRED)
find_package(example_interfaces REQUIRED)
find_package(fastcdr REQUIRED)
find_package(fastrtps REQUIRED)
find_package(moveit_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(rmw REQUIRED)
find_package(trajectory_msgs REQUIRED)
find_package(unique_identifier_msgs REQUIRED)
add_executable(server server.cc)
target_include_directories(server PRIVATE
  ${import_dir}/include
  ${ROOT_DIR}/src/api
  ${ROOT_DIR}/src)
target_link_libraries(server PRIVATE
  action_msgs::action_msgs__rosidl_generator_cpp
  action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp
  builtin_interfaces::builtin_interfaces__rosidl_generator_cpp
  builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp
  control_msgs::control_msgs__rosidl_generator_cpp
  control_msgs::control_msgs__rosidl_typesupport_fastrtps_cpp
  example_interfaces::example_interfaces__rosidl_generator_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
  moveit_msgs::moveit_msgs__rosidl_generator_cpp
  moveit_msgs::moveit_msgs__rosidl_typesupport_fastrtps_cpp
  nav2_msgs::nav2_msgs__rosidl_generator_cpp
  nav2_msgs::nav2_msgs__rosidl_typesupport_fastrtps_cpp
  trajectory_msgs::trajectory_msgs__rosidl_generator_cpp
  trajectory_msgs::trajectory_msgs__rosidl_typesupport_fastrtps_cpp
  unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp
  unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp
  rmw::rmw
  fastrtps
  fastcdr)
CMAKE
}

build_client_if_needed() {
  if [[ "${ACTION_CASE}" == "navigate_to_pose" ]]; then
    CLIENT_BIN="${WORK_DIR}/navigate_to_pose_client.py"
    write_nav2_client "${CLIENT_BIN}"
    return 0
  fi
  if [[ "${ACTION_CASE}" == "follow_path" ]]; then
    CLIENT_BIN="${WORK_DIR}/follow_path_client.py"
    write_follow_path_client "${CLIENT_BIN}"
    return 0
  fi
  if [[ "${ACTION_CASE}" == "navigate_through_poses" ]]; then
    CLIENT_BIN="${WORK_DIR}/navigate_through_poses_client.py"
    write_navigate_through_poses_client "${CLIENT_BIN}"
    return 0
  fi
  if [[ "${ACTION_CASE}" == "execute_trajectory" ]]; then
    CLIENT_BIN="${WORK_DIR}/execute_trajectory_client.py"
    write_execute_trajectory_client "${CLIENT_BIN}"
    return 0
  fi
  if [[ "${ACTION_CASE}" == "move_group" ]]; then
    CLIENT_BIN="${WORK_DIR}/move_group_client.py"
    write_move_group_client "${CLIENT_BIN}"
    return 0
  fi
  if [[ -n "${CLIENT_BIN:-}" ]]; then
    [[ -x "${CLIENT_BIN}" ]] || fail "CLIENT_BIN is not executable: ${CLIENT_BIN}"
    return 0
  fi
  local client_build="${WORK_DIR}/client_build"
  cmake -S "${ROOT_DIR}" -B "${client_build}" \
    -DIBMW_BUILD_WITH_ROS2=ON \
    -DIBMW_BUILD_ROS2_EXTENSION=ON \
    -DIBMW_BUILD_DDS_EXTENSION=ON \
    -DIBMW_BUILD_ROS2_TYPES=ON \
    -DIBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES=ON \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    >"${WORK_DIR}/client_cmake.log" 2>&1
  cmake --build "${client_build}" --target "${CLIENT_BUILD_TARGET}" -j2 \
    >"${WORK_DIR}/client_build.log" 2>&1
  CLIENT_BIN="${client_build}/${CLIENT_BUILD_TARGET}"
  [[ -x "${CLIENT_BIN}" ]] || fail "failed to build native ROS2 action client ${CLIENT_BUILD_TARGET}"
}

run_mode() {
  local mode=$1
  local mode_log_dir="${WORK_DIR}/logs_${mode}"
  mkdir -p "${mode_log_dir}"
  if [[ "${ACTION_CASE}" == "navigate_to_pose" || "${ACTION_CASE}" == "follow_path" || "${ACTION_CASE}" == "navigate_through_poses" || "${ACTION_CASE}" == "execute_trajectory" || "${ACTION_CASE}" == "move_group" ]]; then
    local server_log="${mode_log_dir}/${SERVER_TARGET_NAME}.log"
    local client_log="${mode_log_dir}/${CLIENT_TARGET_NAME}.log"
    local ldd_log="${mode_log_dir}/${SERVER_TARGET_NAME}.ldd.log"
    echo "=========================================="
    echo "ROS2 client -> IBMW DDS-only server validation"
    echo "Validation action: ${VALIDATION_ACTION}"
    echo "Action: ${ACTION_NAME} [${PACKAGE_NAME}/${ROS2_TYPE}]"
    echo "Mode: ${mode}"
    echo "=========================================="
    ldd "${WORK_DIR}/server_build/server" | tee "${ldd_log}"
    if grep -E 'librcl(|cpp|cpp_action)|librmw|librcl_action' "${ldd_log}" >/dev/null 2>&1; then
      fail "server binary links against ROS2 client/runtime libraries"
    fi
    echo "[PASS] Server linkage guard: no rcl/rclcpp/rclcpp_action/rmw dependencies detected"
    : >"${server_log}"
    "${WORK_DIR}/server_build/server" >"${server_log}" 2>&1 &
    local server_pid=$!
    trap 'kill ${server_pid} 2>/dev/null || true; wait ${server_pid} 2>/dev/null || true' RETURN
    local ready=no
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      if grep -F "${SERVER_READY_MARKER}" "${server_log}" >/dev/null 2>&1; then
        ready=yes
        break
      fi
      if ! kill -0 "${server_pid}" 2>/dev/null; then
        cat "${server_log}" >&2 || true
        fail "server exited before readiness marker"
      fi
      sleep 1
    done
    [[ "${ready}" == yes ]] || { cat "${server_log}" >&2 || true; fail "server did not become ready"; }
    set +e
    /usr/bin/python3 "${CLIENT_BIN}" --mode "${mode}" --action-name "${ACTION_NAME}" \
      --cancel-delay-ms "${CANCEL_DELAY_MS}" >"${client_log}" 2>&1
    local client_status=$?
    set -e
    echo "------- client log -------"
    cat "${client_log}"
    echo "------- server log -------"
    cat "${server_log}"
    kill "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
    trap - RETURN
    [[ "${client_status}" -eq 0 ]] || fail "native ROS2 ${ROS2_TYPE} client exited with status ${client_status}"
    grep -F "[PASS]" "${client_log}" >/dev/null 2>&1 || fail "client log did not contain pass marker"
    echo "[PASS] Validation harness completed successfully"
    return 0
  fi
  LOG_DIR="${mode_log_dir}" \
  SERVER_BIN="${WORK_DIR}/server_build/server" \
  CLIENT_BIN="${CLIENT_BIN}" \
  CLIENT_MODE="${mode}" \
  GOAL_ORDER="${GOAL_ORDER}" \
  GRIPPER_POSITION="${GRIPPER_POSITION}" \
  GRIPPER_EFFORT="${GRIPPER_EFFORT}" \
  CANCEL_DELAY_MS="${CANCEL_DELAY_MS}" \
  VALIDATION_ACTION="${VALIDATION_ACTION}" \
  ACTION_NAME="${ACTION_NAME}" \
  SERVER_TARGET_NAME="${SERVER_TARGET_NAME}" \
  CLIENT_TARGET_NAME="${CLIENT_TARGET_NAME}" \
  SERVER_READY_MARKER="${SERVER_READY_MARKER}" \
    bash "${ROOT_DIR}/src/samples/cpp/ros2_action_fibonacci_dds_server/validate_ros2_client_to_ibmw_server.sh" "${mode}"
}

mkdir -p "${WORK_DIR}"
source_ros2

IMPORT_DIR="${WORK_DIR}/import"
SERVER_SRC_DIR="${WORK_DIR}/server_src"
SERVER_BUILD_DIR="${WORK_DIR}/server_build"
rm -rf "${IMPORT_DIR}" "${SERVER_SRC_DIR}" "${SERVER_BUILD_DIR}"
mkdir -p "${SERVER_SRC_DIR}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${PACKAGE_NAME}" \
  --ros2-type "${ROS2_TYPE}" \
  --artifact-only \
  --output-dir "${IMPORT_DIR}" \
  --format json \
  >"${WORK_DIR}/import.json"

case "${ACTION_CASE}" in
  fibonacci)
    write_success_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  gripper_command)
    write_gripper_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  navigate_to_pose)
    write_nav2_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  follow_path)
    write_follow_path_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  navigate_through_poses)
    write_navigate_through_poses_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  execute_trajectory)
    write_execute_trajectory_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
  move_group)
    write_move_group_server_source "${SERVER_SRC_DIR}/server.cc"
    ;;
esac
write_server_cmake "${SERVER_SRC_DIR}/CMakeLists.txt" "${IMPORT_DIR}"
cmake -S "${SERVER_SRC_DIR}" -B "${SERVER_BUILD_DIR}" -DPython3_EXECUTABLE=/usr/bin/python3 \
  >"${WORK_DIR}/server_cmake.log" 2>&1
cmake --build "${SERVER_BUILD_DIR}" -j2 >"${WORK_DIR}/server_build.log" 2>&1

build_client_if_needed
run_mode success
run_mode cancel

echo "[PASS] Generated action success/cancel live acceptance completed"
