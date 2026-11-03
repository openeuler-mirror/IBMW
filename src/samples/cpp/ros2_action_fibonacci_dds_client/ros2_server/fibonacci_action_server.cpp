// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <example_interfaces/action/fibonacci.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

namespace {

using Fibonacci = example_interfaces::action::Fibonacci;
using GoalHandleFibonacci = rclcpp_action::ServerGoalHandle<Fibonacci>;

volatile std::sig_atomic_t g_should_stop = 0;

constexpr int32_t kMaxFibonacciOrder = 47;
constexpr int32_t kMaxConcurrentGoals = 4;

void HandleSignal(int) {
  g_should_stop = 1;
}

struct Options {
  std::string action_name = "/fibonacci";
  int feedback_delay_ms = 50;
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

// Parses the whole of `text` as a base-10 int; reports `flag` on failure.
bool ParseIntArg(const char *flag, const char *text, int &out) {
  const char *end = text + std::strlen(text);
  int value = 0;
  const auto [ptr, ec] = std::from_chars(text, end, value);
  if (*text == '\0' || ec != std::errc() || ptr != end) {
    std::cerr << "[FAIL] Invalid value for " << flag << ": '" << text << "'" << std::endl;
    return false;
  }
  out = value;
  return true;
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
    } else if (arg == "--feedback-delay-ms") {
      const char *value = require_value("--feedback-delay-ms");
      if (value == nullptr) {
        return std::nullopt;
      }
      if (!ParseIntArg("--feedback-delay-ms", value, options.feedback_delay_ms)) {
        return std::nullopt;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "Usage: fibonacci_action_server [options]\n"
          << "  --action-name <name>        Action name (default: /fibonacci)\n"
          << "  --feedback-delay-ms <ms>    Delay between feedback frames (default: 50)\n"
          << "  --help                      Show this message\n";
      options.show_help = true;
      return options;
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      return std::nullopt;
    }
  }

  if (options.feedback_delay_ms < 0) {
    std::cerr << "--feedback-delay-ms must be >= 0" << std::endl;
    return std::nullopt;
  }

  return options;
}

int Fail(const std::string &message) {
  std::cerr << "[FAIL] " << message << std::endl;
  return 1;
}

class FibonacciActionServer : public rclcpp::Node {
 public:
  explicit FibonacciActionServer(Options options)
      : rclcpp::Node("ibmw_fibonacci_action_ros2_server"), options_(std::move(options)) {
    server_ = rclcpp_action::create_server<Fibonacci>(
        this,
        options_.action_name,
        [this](const rclcpp_action::GoalUUID &, std::shared_ptr<const Fibonacci::Goal> goal) {
          return HandleGoal(goal);
        },
        [this](const std::shared_ptr<GoalHandleFibonacci> &goal_handle) {
          return HandleCancel(goal_handle);
        },
        [this](const std::shared_ptr<GoalHandleFibonacci> &goal_handle) {
          HandleAccepted(goal_handle);
        });
  }

  ~FibonacciActionServer() override { JoinWorkers(); }

  void PrintReady() const {
    std::cout << "SERVER_READY action_name=" << options_.action_name
              << " feedback_delay_ms=" << options_.feedback_delay_ms << std::endl;
    std::cout.flush();
  }

  void Stop() { JoinWorkers(); }

 private:
  rclcpp_action::GoalResponse HandleGoal(const std::shared_ptr<const Fibonacci::Goal> &goal) {
    if (goal->order <= 0 || goal->order > kMaxFibonacciOrder) {
      std::cout << "GOAL_REJECTED order=" << goal->order << std::endl;
      std::cout.flush();
      return rclcpp_action::GoalResponse::REJECT;
    }
    const int32_t active = active_goals_.load();
    if (active >= kMaxConcurrentGoals) {
      std::cout << "GOAL_REJECTED capacity=" << active << std::endl;
      std::cout.flush();
      return rclcpp_action::GoalResponse::REJECT;
    }
    active_goals_.fetch_add(1);
    std::cout << "GOAL_ACCEPTED order=" << goal->order << std::endl;
    std::cout.flush();
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }

  rclcpp_action::CancelResponse HandleCancel(const std::shared_ptr<GoalHandleFibonacci> &) {
    std::cout << "GOAL_CANCELING" << std::endl;
    std::cout.flush();
    return rclcpp_action::CancelResponse::ACCEPT;
  }

  void HandleAccepted(const std::shared_ptr<GoalHandleFibonacci> &goal_handle) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.emplace_back([this, goal_handle] { Execute(goal_handle); });
  }

  void Execute(const std::shared_ptr<GoalHandleFibonacci> &goal_handle) const {
    struct ActiveGoalGuard {
      explicit ActiveGoalGuard(std::atomic<int32_t> &active) : active(active) {}
      ~ActiveGoalGuard() { active.fetch_sub(1); }
      std::atomic<int32_t> &active;
    } guard(active_goals_);

    const auto goal = goal_handle->get_goal();
    auto feedback = std::make_shared<Fibonacci::Feedback>();
    auto result = std::make_shared<Fibonacci::Result>();

    if (goal->order <= 0) {
      result->sequence.clear();
      goal_handle->succeed(result);
      std::cout << "GOAL_SUCCEEDED sequence=[]" << std::endl;
      std::cout.flush();
      return;
    }

    std::vector<int32_t> sequence;
    sequence.reserve(static_cast<std::size_t>(goal->order));
    sequence.push_back(0);
    if (goal->order > 1) {
      sequence.push_back(1);
    }

    for (int32_t index = static_cast<int32_t>(sequence.size()); index < goal->order; ++index) {
      if (goal_handle->is_canceling()) {
        result->sequence = sequence;
        goal_handle->canceled(result);
        std::cout << "GOAL_CANCELED sequence=" << JoinSequence(result->sequence) << std::endl;
        std::cout.flush();
        return;
      }

      sequence.push_back(sequence[static_cast<std::size_t>(index) - 1U] +
                         sequence[static_cast<std::size_t>(index) - 2U]);
      feedback->sequence = sequence;
      goal_handle->publish_feedback(feedback);
      std::cout << "FEEDBACK_SENT sequence=" << JoinSequence(sequence) << std::endl;
      std::cout.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(options_.feedback_delay_ms));
    }

    if (goal_handle->is_canceling()) {
      result->sequence = sequence;
      goal_handle->canceled(result);
      std::cout << "GOAL_CANCELED sequence=" << JoinSequence(result->sequence) << std::endl;
      std::cout.flush();
      return;
    }

    result->sequence = sequence;
    goal_handle->succeed(result);
    std::cout << "GOAL_SUCCEEDED sequence=" << JoinSequence(result->sequence) << std::endl;
    std::cout.flush();
  }

  void JoinWorkers() {
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(workers_mutex_);
      workers.swap(workers_);
    }
    for (auto &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  Options options_;
  rclcpp_action::Server<Fibonacci>::SharedPtr server_;
  mutable std::atomic<int32_t> active_goals_{0};
  std::mutex workers_mutex_;
  std::vector<std::thread> workers_;
};

}  // namespace

int main(int argc, char *argv[]) {
  const auto options = ParseArgs(argc, argv);
  if (!options.has_value()) {
    return 2;
  }
  if (options->show_help) {
    return 0;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  rclcpp::init(argc, argv);
  auto server = std::make_shared<FibonacciActionServer>(*options);
  server->PrintReady();

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(server);

  while (rclcpp::ok() && g_should_stop == 0) {
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  server->Stop();
  rclcpp::shutdown();
  return 0;
}
