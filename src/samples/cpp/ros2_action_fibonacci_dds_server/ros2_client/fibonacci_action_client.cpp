// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <example_interfaces/action/fibonacci.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

namespace {

using Fibonacci = example_interfaces::action::Fibonacci;
using GoalHandleFibonacci = rclcpp_action::ClientGoalHandle<Fibonacci>;

constexpr int32_t kMaxFibonacciOrder = 47;

struct Options {
  std::string action_name = "/fibonacci";
  std::string mode = "success";
  int order = 8;
  int cancel_delay_ms = 250;
  int server_wait_seconds = 10;
  int result_wait_seconds = 20;
  bool show_help = false;
};

std::string JoinSequence(const std::vector<int32_t> &sequence) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < sequence.size(); ++i) {
    if (i != 0U) {
      oss << ", ";
    }
    oss << sequence[i];
  }
  oss << "]";
  return oss.str();
}

std::vector<int32_t> BuildExpectedSequence(int order) {
  std::vector<int32_t> expected;
  expected.reserve(static_cast<std::size_t>(order));
  if (order >= 1) {
    expected.push_back(0);
  }
  if (order >= 2) {
    expected.push_back(1);
  }
  for (int i = 2; i < order; ++i) {
    expected.push_back(expected[static_cast<std::size_t>(i - 1)] + expected[static_cast<std::size_t>(i - 2)]);
  }
  return expected;
}

bool IsExpectedSequence(const std::vector<int32_t> &sequence, int order) {
  return sequence == BuildExpectedSequence(order);
}

std::optional<Options> ParseArgs(int argc, char *argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << flag << std::endl;
        return nullptr;
      }
      ++i;
      return argv[i];
    };

    if (arg == "--action-name") {
      const char *value = require_value("--action-name");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.action_name = value;
    } else if (arg == "--mode") {
      const char *value = require_value("--mode");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.mode = value;
    } else if (arg == "--order") {
      const char *value = require_value("--order");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.order = std::stoi(value);
    } else if (arg == "--cancel-delay-ms") {
      const char *value = require_value("--cancel-delay-ms");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.cancel_delay_ms = std::stoi(value);
    } else if (arg == "--server-wait-seconds") {
      const char *value = require_value("--server-wait-seconds");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.server_wait_seconds = std::stoi(value);
    } else if (arg == "--result-wait-seconds") {
      const char *value = require_value("--result-wait-seconds");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.result_wait_seconds = std::stoi(value);
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: fibonacci_action_client [options]\n"
          << "  --mode <success|cancel|reject> Validation case (default: success)\n"
          << "  --order <n>                    Goal order (default: 8)\n"
          << "  --action-name <name>           Action name (default: /fibonacci)\n"
          << "  --cancel-delay-ms <ms>         Delay before cancel in cancel mode (default: 250)\n"
          << "  --server-wait-seconds <sec>    Wait for action server (default: 10)\n"
          << "  --result-wait-seconds <sec>    Wait for terminal result (default: 20)\n";
      options.show_help = true;
      return options;
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      return std::nullopt;
    }
  }

  if (options.mode != "success" && options.mode != "cancel" && options.mode != "reject") {
    std::cerr << "Unsupported mode: " << options.mode << std::endl;
    return std::nullopt;
  }
  if (options.order <= 0 || options.order > kMaxFibonacciOrder) {
    std::cerr << "--order must be in range 1.." << kMaxFibonacciOrder << std::endl;
    return std::nullopt;
  }
  if (options.server_wait_seconds <= 0 || options.result_wait_seconds <= 0 ||
      options.cancel_delay_ms < 0) {
    std::cerr << "Invalid timeout values" << std::endl;
    return std::nullopt;
  }

  return options;
}

int Fail(const std::string &message) {
  std::cerr << "[FAIL] " << message << std::endl;
  return 1;
}

struct RosScope {
  RosScope(int argc, char *argv[]) { rclcpp::init(argc, argv); }
  ~RosScope() { rclcpp::shutdown(); }
};

template <typename FutureT>
bool SpinUntilReady(rclcpp::Executor &executor,
                    const FutureT &future,
                    const std::chrono::steady_clock::time_point &deadline) {
  while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

}  // namespace

int main(int argc, char *argv[]) {
  const auto options = ParseArgs(argc, argv);
  if (!options.has_value()) {
    return 2;
  }
  if (options->show_help) {
    return 0;
  }

  RosScope ros_scope(argc, argv);

  auto node = rclcpp::Node::make_shared("ibmw_fibonacci_action_ros2_client");
  auto client = rclcpp_action::create_client<Fibonacci>(node, options->action_name);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  std::cout << "[INFO] action_name=" << options->action_name
            << " mode=" << options->mode
            << " order=" << options->order << std::endl;

  if (!client->wait_for_action_server(std::chrono::seconds(options->server_wait_seconds))) {
    return Fail("action server not available within timeout");
  }

  Fibonacci::Goal goal;
  goal.order = options->order;

  std::promise<void> goal_response_promise;
  std::promise<GoalHandleFibonacci::WrappedResult> result_promise;
  auto goal_response_future = goal_response_promise.get_future();
  auto result_future = result_promise.get_future();

  bool saw_feedback = false;
  std::vector<int32_t> last_feedback_sequence;

  rclcpp_action::Client<Fibonacci>::SendGoalOptions send_goal_options;
  send_goal_options.goal_response_callback =
      [&goal_response_promise](const GoalHandleFibonacci::SharedPtr &goal_handle) mutable {
        std::cout << (goal_handle ? "[INFO] Goal accepted" : "[INFO] Goal rejected") << std::endl;
        goal_response_promise.set_value();
      };
  send_goal_options.feedback_callback =
      [&saw_feedback, &last_feedback_sequence](GoalHandleFibonacci::SharedPtr,
                                               const std::shared_ptr<const Fibonacci::Feedback> &feedback) mutable {
        saw_feedback = true;
        last_feedback_sequence = feedback->sequence;
        std::cout << "[INFO] Feedback sequence=" << JoinSequence(last_feedback_sequence) << std::endl;
      };
  send_goal_options.result_callback =
      [&result_promise](const GoalHandleFibonacci::WrappedResult &result) mutable {
        result_promise.set_value(result);
      };

  auto send_goal_future = client->async_send_goal(goal, send_goal_options);
  const auto goal_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(options->result_wait_seconds);
  if (!SpinUntilReady(executor, goal_response_future, goal_deadline)) {
    return Fail("timed out waiting for goal response callback");
  }

  auto goal_handle = send_goal_future.get();
  if (options->mode == "reject") {
    if (goal_handle) {
      return Fail("expected goal rejection from server");
    }
    std::cout << "[PASS] Reject case observed goal rejection" << std::endl;
    return 0;
  }
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
    const auto &result_sequence = wrapped_result.result->sequence;
    std::cout << "[INFO] Result sequence=" << JoinSequence(result_sequence) << std::endl;
    if (!IsExpectedSequence(result_sequence, options->order)) {
      return Fail("result sequence does not equal the expected Fibonacci order");
    }
    if (!saw_feedback) {
      return Fail("expected at least one feedback callback before success result");
    }
    if (!last_feedback_sequence.empty()) {
      if (last_feedback_sequence.size() > result_sequence.size()) {
        return Fail("feedback sequence was longer than final result sequence");
      }
      for (std::size_t i = 0; i < last_feedback_sequence.size(); ++i) {
        if (last_feedback_sequence[i] != result_sequence[i]) {
          return Fail("feedback sequence was not a prefix of the final result sequence");
        }
      }
    }
    std::cout << "[PASS] Success case matched deterministic Fibonacci result" << std::endl;
    return 0;
  }

  if (wrapped_result.code != rclcpp_action::ResultCode::CANCELED) {
    return Fail("expected CANCELED result code in cancel mode");
  }
  std::cout << "[PASS] Cancel case reached terminal CANCELED status" << std::endl;
  return 0;
}
