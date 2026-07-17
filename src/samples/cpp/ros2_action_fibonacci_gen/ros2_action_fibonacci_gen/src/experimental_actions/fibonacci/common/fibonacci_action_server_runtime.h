// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_SERVER_RUNTIME_H_
#define IBMW_SAMPLES_CPP_ROS2_ACTION_FIBONACCI_COMMON_FIBONACCI_ACTION_SERVER_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpp/action/server_runtime.h"
#include "fibonacci_action_common.h"

namespace ibmw::samples::ros2_action_fibonacci {

constexpr int8_t kGoalStatusUnknown = ibmw::action::kGoalStatusUnknown;
constexpr int8_t kGoalStatusAccepted = ibmw::action::kGoalStatusAccepted;
constexpr int8_t kGoalStatusExecuting = ibmw::action::kGoalStatusExecuting;
constexpr int8_t kGoalStatusCanceling = ibmw::action::kGoalStatusCanceling;
constexpr int8_t kGoalStatusSucceeded = ibmw::action::kGoalStatusSucceeded;
constexpr int8_t kGoalStatusCanceled = ibmw::action::kGoalStatusCanceled;
constexpr int8_t kCancelReturnNone = ibmw::action::kCancelReturnNone;
constexpr int8_t kCancelReturnUnknownGoalId = ibmw::action::kCancelReturnUnknownGoalId;

enum class GoalAcceptanceFailure {
  kNone,
  kInvalidOrder,
  kDuplicateGoal,
  kCapacityFull,
};

struct FibonacciHook {
  using GoalInput = int32_t;
  using FeedbackPayload = std::vector<int32_t>;
  using ResultPayload = std::vector<int32_t>;

  struct ExecState {
    int32_t order = 0;
    std::vector<int32_t> sequence;
  };

  static bool ValidateGoal(const GoalInput& order) { return IsValidGoalOrder(order); }

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

template <typename UuidT, typename TimeT, typename PendingResultT,
          typename ClockT = std::chrono::steady_clock>
class FibonacciActionServerRuntime {
 public:
  using TimePoint = typename ClockT::time_point;

  struct AcceptGoalDecision {
    bool accepted = false;
    GoalAcceptanceFailure failure = GoalAcceptanceFailure::kNone;
    std::string key;
  };

  struct GetResultDecision {
    bool immediate = false;
    int8_t status = kGoalStatusUnknown;
    std::vector<int32_t> sequence;
    bool queue_full = false;
  };

  struct GoalInfoEntry {
    UuidT goal_id;
    TimeT stamp;
  };

  struct CancelDecision {
    int8_t return_code = kCancelReturnUnknownGoalId;
    std::vector<GoalInfoEntry> goals_canceling;
  };

  struct FeedbackSnapshot {
    UuidT goal_id;
    std::vector<int32_t> sequence;
  };

  struct ExecutionTransition {
    bool found = false;
    bool terminal = false;
    int8_t final_status = kGoalStatusUnknown;
    std::vector<int32_t> final_sequence;
    std::vector<PendingResultT> pending_results;
  };

  struct StatusEntry {
    UuidT goal_id;
    TimeT stamp;
    int8_t status = kGoalStatusUnknown;
  };

  struct StatusSnapshot {
    std::vector<StatusEntry> status_list;
    std::vector<std::string> pruned_keys;
  };

  AcceptGoalDecision AcceptGoal(const UuidT& goal_id, const TimeT& stamp, int32_t order) {
    const auto decision = runtime_.AcceptGoal(goal_id, stamp, order, kDefaultMaxConcurrentGoals,
                                              kDefaultMaxRetainedGoals);
    AcceptGoalDecision out;
    out.accepted = decision.accepted;
    out.failure = ToFibonacciFailure(decision.failure);
    out.key = decision.key;
    return out;
  }

  GetResultDecision GetResult(
      const UuidT& goal_id, PendingResultT pending_result,
      std::size_t max_pending_results_per_goal = kDefaultMaxPendingResultsPerGoal) {
    const auto decision =
        runtime_.GetResult(goal_id, std::move(pending_result), max_pending_results_per_goal);
    GetResultDecision out;
    out.immediate = decision.immediate;
    out.status = decision.status;
    out.sequence = decision.result;
    out.queue_full = decision.queue_full;
    return out;
  }

  CancelDecision CancelGoal(const UuidT& goal_id, const TimeT& stamp) {
    const auto decision = runtime_.CancelGoal(goal_id, stamp);
    CancelDecision out;
    out.return_code = decision.return_code;
    for (const auto& goal : decision.goals_canceling) {
      out.goals_canceling.push_back(GoalInfoEntry{.goal_id = goal.goal_id, .stamp = goal.stamp});
    }
    return out;
  }

  bool StartExecuting(const std::string& key) { return runtime_.StartExecuting(key); }

  ExecutionTransition AdvanceExecution(const std::string& key, TimePoint now) {
    const auto transition = runtime_.AdvanceExecution(key, now);
    ExecutionTransition out;
    out.found = transition.found;
    out.terminal = transition.terminal;
    out.final_status = transition.final_status;
    out.final_sequence = transition.final_result;
    out.pending_results = std::move(transition.pending_results);
    return out;
  }

  std::optional<FeedbackSnapshot> SnapshotFeedback(const std::string& key) const {
    const auto snapshot = runtime_.SnapshotFeedback(key);
    if (!snapshot.has_value()) {
      return std::nullopt;
    }
    return FeedbackSnapshot{.goal_id = snapshot->goal_id, .sequence = snapshot->feedback};
  }

  StatusSnapshot SnapshotStatusAndPrune(TimePoint now,
                                        std::chrono::seconds retention = std::chrono::seconds(60)) {
    const auto snapshot = runtime_.SnapshotStatusAndPrune(now, retention);
    StatusSnapshot out;
    out.pruned_keys = snapshot.pruned_keys;
    for (const auto& entry : snapshot.status_list) {
      out.status_list.push_back(
          StatusEntry{.goal_id = entry.goal_id, .stamp = entry.stamp, .status = entry.status});
    }
    return out;
  }

  void RequestStopAll() { runtime_.RequestStopAll(); }

  bool HasGoal(const UuidT& goal_id) const { return runtime_.HasGoal(goal_id); }

 private:
  static GoalAcceptanceFailure ToFibonacciFailure(ibmw::action::GoalAcceptanceFailure failure) {
    switch (failure) {
      case ibmw::action::GoalAcceptanceFailure::kInvalidGoal:
        return GoalAcceptanceFailure::kInvalidOrder;
      case ibmw::action::GoalAcceptanceFailure::kDuplicateGoal:
        return GoalAcceptanceFailure::kDuplicateGoal;
      case ibmw::action::GoalAcceptanceFailure::kCapacityFull:
        return GoalAcceptanceFailure::kCapacityFull;
      case ibmw::action::GoalAcceptanceFailure::kNone:
        return GoalAcceptanceFailure::kNone;
    }
    return GoalAcceptanceFailure::kNone;
  }

  ibmw::action::ServerRuntime<FibonacciHook, UuidT, TimeT, PendingResultT, ClockT> runtime_;
};

}  // namespace ibmw::samples::ros2_action_fibonacci

#endif
