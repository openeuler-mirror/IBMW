// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <control_msgs/action/gripper_command.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

namespace {

using GripperCommand = control_msgs::action::GripperCommand;
using GoalHandleGripperCommand = rclcpp_action::ClientGoalHandle<GripperCommand>;

struct Options {
  std::string action_name = "/gripper_command";
  std::string mode = "success";
  double position = 0.42;
  double effort = 3.0;
  int cancel_delay_ms = 0;
  int server_wait_seconds = 10;
  int result_wait_seconds = 20;
  bool show_help = false;
};

std::optional<Options> ParseArgs(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << flag << std::endl;
        return nullptr;
      }
      ++i;
      return argv[i];
    };

    if (arg == "--action-name") {
      const char* value = require_value("--action-name");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.action_name = value;
    } else if (arg == "--mode") {
      const char* value = require_value("--mode");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.mode = value;
    } else if (arg == "--position") {
      const char* value = require_value("--position");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.position = std::stod(value);
    } else if (arg == "--effort") {
      const char* value = require_value("--effort");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.effort = std::stod(value);
    } else if (arg == "--cancel-delay-ms") {
      const char* value = require_value("--cancel-delay-ms");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.cancel_delay_ms = std::stoi(value);
    } else if (arg == "--server-wait-seconds") {
      const char* value = require_value("--server-wait-seconds");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.server_wait_seconds = std::stoi(value);
    } else if (arg == "--result-wait-seconds") {
      const char* value = require_value("--result-wait-seconds");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.result_wait_seconds = std::stoi(value);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: gripper_command_action_client [options]\n"
                << "  --mode <success|cancel>        Validation case (default: success)\n"
                << "  --position <meters>            Target position in [0, 1] (default: 0.42)\n"
                << "  --effort <newtons>             Max effort >= 0 (default: 3.0)\n"
                << "  --action-name <name>           Action name (default: /gripper_command)\n"
                << "  --cancel-delay-ms <ms>         Delay before cancel in cancel mode (default: 0)\n"
                << "  --server-wait-seconds <sec>    Wait for action server (default: 10)\n"
                << "  --result-wait-seconds <sec>    Wait for terminal result (default: 20)\n";
      options.show_help = true;
      return options;
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      return std::nullopt;
    }
  }

  if (options.mode != "success" && options.mode != "cancel") {
    std::cerr << "Unsupported mode: " << options.mode << std::endl;
    return std::nullopt;
  }
  if (!std::isfinite(options.position) || !std::isfinite(options.effort) || options.position < 0.0 ||
      options.position > 1.0 || options.effort < 0.0) {
    std::cerr << "Invalid gripper command values" << std::endl;
    return std::nullopt;
  }
  if (options.server_wait_seconds <= 0 || options.result_wait_seconds <= 0 ||
      options.cancel_delay_ms < 0) {
    std::cerr << "Invalid timeout values" << std::endl;
    return std::nullopt;
  }

  return options;
}

int Fail(const std::string& message) {
  std::cerr << "[FAIL] " << message << std::endl;
  return 1;
}

struct RosScope {
  RosScope(int argc, char* argv[]) { rclcpp::init(argc, argv); }
  ~RosScope() { rclcpp::shutdown(); }
};

template <typename FutureT>
bool SpinUntilReady(rclcpp::Executor& executor,
                    const FutureT& future,
                    const std::chrono::steady_clock::time_point& deadline) {
  while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

bool CloseEnough(double left, double right) {
  return std::abs(left - right) <= 1e-6;
}

}  // namespace

int main(int argc, char* argv[]) {
  const auto options = ParseArgs(argc, argv);
  if (!options.has_value()) {
    return 2;
  }
  if (options->show_help) {
    return 0;
  }

  RosScope ros_scope(argc, argv);

  auto node = rclcpp::Node::make_shared("ibmw_gripper_command_action_ros2_client");
  auto client = rclcpp_action::create_client<GripperCommand>(node, options->action_name);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  std::cout << "[INFO] action_name=" << options->action_name
            << " mode=" << options->mode
            << " position=" << options->position
            << " effort=" << options->effort << std::endl;

  if (!client->wait_for_action_server(std::chrono::seconds(options->server_wait_seconds))) {
    return Fail("action server not available within timeout");
  }

  GripperCommand::Goal goal;
  goal.command.position = options->position;
  goal.command.max_effort = options->effort;

  std::promise<void> goal_response_promise;
  std::promise<GoalHandleGripperCommand::WrappedResult> result_promise;
  auto goal_response_future = goal_response_promise.get_future();
  auto result_future = result_promise.get_future();

  bool saw_feedback = false;
  GripperCommand::Feedback last_feedback;

  rclcpp_action::Client<GripperCommand>::SendGoalOptions send_goal_options;
  send_goal_options.goal_response_callback =
      [&goal_response_promise](const GoalHandleGripperCommand::SharedPtr& goal_handle) mutable {
        std::cout << (goal_handle ? "[INFO] Goal accepted" : "[INFO] Goal rejected") << std::endl;
        goal_response_promise.set_value();
      };
  send_goal_options.feedback_callback =
      [&saw_feedback, &last_feedback](GoalHandleGripperCommand::SharedPtr,
                                      const std::shared_ptr<const GripperCommand::Feedback>& feedback) mutable {
        saw_feedback = true;
        last_feedback = *feedback;
        std::cout << "[INFO] Feedback position=" << last_feedback.position
                  << " effort=" << last_feedback.effort
                  << " reached_goal=" << last_feedback.reached_goal << std::endl;
      };
  send_goal_options.result_callback =
      [&result_promise](const GoalHandleGripperCommand::WrappedResult& result) mutable {
        result_promise.set_value(result);
      };

  auto send_goal_future = client->async_send_goal(goal, send_goal_options);
  const auto goal_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(options->result_wait_seconds);
  if (!SpinUntilReady(executor, goal_response_future, goal_deadline)) {
    return Fail("timed out waiting for goal response callback");
  }

  auto goal_handle = send_goal_future.get();
  if (!goal_handle) {
    return Fail("goal was rejected by server");
  }

  if (options->mode == "cancel") {
    std::this_thread::sleep_for(std::chrono::milliseconds(options->cancel_delay_ms));
    auto cancel_future = client->async_cancel_goal(goal_handle);
    const auto cancel_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(options->result_wait_seconds);
    if (!SpinUntilReady(executor, cancel_future, cancel_deadline)) {
      return Fail("timed out waiting for cancel response");
    }

    const auto cancel_response = cancel_future.get();
    std::cout << "[INFO] Cancel return_code=" << cancel_response->return_code
              << " goals_canceling=" << cancel_response->goals_canceling.size() << std::endl;
    if (cancel_response->return_code != 0) {
      return Fail("cancel request was not accepted");
    }
  }

  const auto result_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(options->result_wait_seconds);
  if (!SpinUntilReady(executor, result_future, result_deadline)) {
    return Fail("timed out waiting for terminal result");
  }

  const auto wrapped_result = result_future.get();
  if (options->mode == "success") {
    if (wrapped_result.code != rclcpp_action::ResultCode::SUCCEEDED) {
      return Fail("expected SUCCEEDED result code");
    }
    const auto& result = wrapped_result.result;
    std::cout << "[INFO] Result position=" << result->position
              << " effort=" << result->effort
              << " reached_goal=" << result->reached_goal << std::endl;
    if (!CloseEnough(result->position, options->position) || !CloseEnough(result->effort, options->effort) ||
        result->stalled || !result->reached_goal) {
      return Fail("result fields did not match the expected gripper command outcome");
    }
    if (!saw_feedback) {
      return Fail("expected at least one feedback callback before success result");
    }
    if (last_feedback.position < 0.0 || last_feedback.position > options->position) {
      return Fail("feedback position was outside expected range");
    }
    std::cout << "[PASS] Success case matched deterministic GripperCommand result" << std::endl;
    return 0;
  }

  if (wrapped_result.code != rclcpp_action::ResultCode::CANCELED) {
    return Fail("expected CANCELED result code in cancel mode");
  }
  std::cout << "[PASS] Cancel case reached terminal CANCELED status" << std::endl;
  return 0;
}
