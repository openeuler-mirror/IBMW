// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) server runtime.
//
// ibmw::action::ServerRuntime<Hook> is the generic, action-independent goal
// lifecycle engine shared by every DDS-only action server. It owns the
// internal ActionGoalLedger (accept / get-result / cancel / terminal / status /
// prune bookkeeping) and a per-goal execution-state map keyed by the canonical
// goal-id string. All action-specific policy (goal validation, execution
// seeding, per-tick advancement, feedback / result construction) is delegated
// to a user-supplied Hook type.
//
// The Hook contract (associated types + static policy methods):
//
//   struct Hook {
//     // Per-goal mutable execution state owned by the runtime:
//     struct ExecState { ... };
//
//     // Action-specific feedback / result payloads surfaced by the runtime.
//     // These are plain value types (NOT ROS 2 message structs); the server
//     // .cpp converts them into wire messages.
//     using FeedbackPayload = ...;
//     using ResultPayload = ...;
//
//     // The goal request input the runtime forwards to ValidateGoal/InitState.
//     using GoalInput = ...;
//
//     // Reject invalid goals before they ever reach the ledger.
//     static bool ValidateGoal(const GoalInput& goal);
//
//     // Build the initial execution state once the ledger has accepted a goal.
//     static ExecState InitState(const GoalInput& goal);
//
//     // Advance one execution tick. Returns a TickOutcome telling the runtime
//     // whether to keep ticking or transition to a terminal status.
//     //
//     // CONTRACT INVARIANT: Tick must NOT mutate state on any call where it
//     // returns a terminal outcome (kSucceeded / kCanceled / kAborted). The
//     // runtime reads the post-Tick state via BuildResult to produce the final
//     // payload; mutating on a terminal tick would fold one extra step into the
//     // reported result. Check the terminal condition first and early-return.
//     static TickOutcome Tick(ExecState& state);
//
//     // Snapshot the current feedback / final result payloads.
//     static FeedbackPayload BuildFeedback(const ExecState& state);
//     static ResultPayload BuildResult(const ExecState& state);
//   };
//
// Field naming is deliberately generic (result / final_result). Per-action
// wrappers (e.g. the Fibonacci runtime) re-expose these under their own
// domain names (sequence / final_sequence) so existing call sites and tests
// stay byte-for-byte stable.

#ifndef IBMW_API_CPP_ACTION_SERVER_RUNTIME_H_
#define IBMW_API_CPP_ACTION_SERVER_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal/ros2_action_dds/action_goal_ledger.h"
#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::action {

// Canonical ROS 2 GoalStatus values (action_msgs/msg/GoalStatus) and
// CancelGoal return codes (action_msgs/srv/CancelGoal). These are wire-stable
// constants of the ROS 2 action protocol and therefore live with the generic
// runtime rather than any per-action layer.
inline constexpr int8_t kGoalStatusUnknown = 0;
inline constexpr int8_t kGoalStatusAccepted = 1;
inline constexpr int8_t kGoalStatusExecuting = 2;
inline constexpr int8_t kGoalStatusCanceling = 3;
inline constexpr int8_t kGoalStatusSucceeded = 4;
inline constexpr int8_t kGoalStatusCanceled = 5;
inline constexpr int8_t kGoalStatusAborted = 6;
inline constexpr int8_t kCancelReturnNone = 0;
inline constexpr int8_t kCancelReturnUnknownGoalId = 2;

inline constexpr std::size_t kDefaultMaxConcurrentGoals = 4;
inline constexpr std::size_t kDefaultMaxRetainedGoals = 32;
inline constexpr std::size_t kDefaultMaxPendingResultsPerGoal = 16;

enum class GoalAcceptanceFailure {
  kNone,
  kInvalidGoal,
  kDuplicateGoal,
  kCapacityFull,
};

// Outcome of a single Hook::Tick step. kContinue keeps executing (runtime
// publishes feedback); the others transition the goal to a terminal status.
enum class TickOutcome {
  kContinue,
  kSucceeded,
  kCanceled,
  kAborted,
};

template <typename Hook, typename UuidT, typename TimeT, typename PendingResultT,
          typename ClockT = std::chrono::steady_clock>
class ServerRuntime {
 public:
  using TimePoint = typename ClockT::time_point;
  using Ledger =
      ibmw::internal::ros2_action_dds::ActionGoalLedger<UuidT, TimeT, PendingResultT, ClockT>;
  using ExecState = typename Hook::ExecState;
  using FeedbackPayload = typename Hook::FeedbackPayload;
  using ResultPayload = typename Hook::ResultPayload;
  using GoalInput = typename Hook::GoalInput;

  struct AcceptGoalDecision {
    bool accepted = false;
    GoalAcceptanceFailure failure = GoalAcceptanceFailure::kNone;
    std::string key;
  };

  struct GetResultDecision {
    bool immediate = false;
    int8_t status = kGoalStatusUnknown;
    ResultPayload result{};
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
    FeedbackPayload feedback{};
  };

  struct ExecutionTransition {
    bool found = false;
    bool terminal = false;
    int8_t final_status = kGoalStatusUnknown;
    ResultPayload final_result{};
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

  AcceptGoalDecision AcceptGoal(const UuidT& goal_id, const TimeT& stamp, const GoalInput& goal,
                                std::size_t max_concurrent_goals = kDefaultMaxConcurrentGoals,
                                std::size_t max_retained_goals = kDefaultMaxRetainedGoals) {
    AcceptGoalDecision decision;
    decision.key = ibmw::internal::ros2_action_dds::UuidToString(goal_id);
    if (!Hook::ValidateGoal(goal)) {
      decision.failure = GoalAcceptanceFailure::kInvalidGoal;
      return decision;
    }
    const auto ledger_decision = ledger_.AcceptGoal(goal_id, stamp, kGoalStatusAccepted,
                                                    max_concurrent_goals, max_retained_goals);
    decision.key = ledger_decision.key;
    if (ledger_decision.code ==
        ibmw::internal::ros2_action_dds::ActionGoalAcceptCode::kDuplicateGoal) {
      decision.failure = GoalAcceptanceFailure::kDuplicateGoal;
      return decision;
    }
    if (ledger_decision.code ==
        ibmw::internal::ros2_action_dds::ActionGoalAcceptCode::kCapacityFull) {
      decision.failure = GoalAcceptanceFailure::kCapacityFull;
      return decision;
    }

    pending_inputs_.emplace(decision.key, goal);
    decision.accepted = true;
    return decision;
  }

  GetResultDecision GetResult(
      const UuidT& goal_id, PendingResultT pending_result,
      std::size_t max_pending_results_per_goal = kDefaultMaxPendingResultsPerGoal) {
    const auto key = ibmw::internal::ros2_action_dds::UuidToString(goal_id);
    const auto ledger_decision =
        ledger_.GetResult(goal_id, std::move(pending_result), max_pending_results_per_goal);
    if (!ledger_decision.immediate) {
      return GetResultDecision{};
    }
    if (ledger_decision.code ==
        ibmw::internal::ros2_action_dds::ActionGetResultCode::kPendingQueueFull) {
      return GetResultDecision{
          .immediate = true, .status = kGoalStatusUnknown, .result = {}, .queue_full = true};
    }
    if (!ledger_.HasGoal(goal_id)) {
      return GetResultDecision{
          .immediate = true, .status = kGoalStatusUnknown, .result = {}, .queue_full = false};
    }
    return GetResultDecision{.immediate = true,
                             .status = ledger_decision.status,
                             .result = ResultForKey(key),
                             .queue_full = false};
  }

  CancelDecision CancelGoal(const UuidT& goal_id, const TimeT& stamp) {
    CancelDecision decision;
    const auto ledger_decision = ledger_.CancelGoal(goal_id, stamp, kGoalStatusCanceling,
                                                    kCancelReturnNone, kCancelReturnUnknownGoalId);
    decision.return_code = ledger_decision.return_code;
    for (const auto& goal : ledger_decision.goals_canceling) {
      decision.goals_canceling.push_back(GoalInfoEntry{.goal_id = goal.goal_id, .stamp = goal.stamp});
    }
    return decision;
  }

  bool StartExecuting(const std::string& key) {
    if (!ledger_.SetExecuting(key, kGoalStatusExecuting)) {
      return false;
    }
    const auto input_it = pending_inputs_.find(key);
    if (input_it == pending_inputs_.end()) {
      return false;
    }
    states_.insert_or_assign(key, Hook::InitState(input_it->second));
    return true;
  }

  ExecutionTransition AdvanceExecution(const std::string& key, TimePoint now) {
    ExecutionTransition transition;
    auto state_it = states_.find(key);
    if (state_it == states_.end()) {
      return transition;
    }
    transition.found = true;
    auto& state = state_it->second;
    if (ledger_.IsCancelRequested(key)) {
      FinalizeTerminal(transition, key, kGoalStatusCanceled, state, now);
      return transition;
    }
    const TickOutcome outcome = Hook::Tick(state);
    switch (outcome) {
      case TickOutcome::kContinue:
        break;
      case TickOutcome::kSucceeded:
        FinalizeTerminal(transition, key, kGoalStatusSucceeded, state, now);
        break;
      case TickOutcome::kCanceled:
        FinalizeTerminal(transition, key, kGoalStatusCanceled, state, now);
        break;
      case TickOutcome::kAborted:
        FinalizeTerminal(transition, key, kGoalStatusAborted, state, now);
        break;
    }
    return transition;
  }

  std::optional<FeedbackSnapshot> SnapshotFeedback(const std::string& key) const {
    auto it = states_.find(key);
    if (it == states_.end()) {
      return std::nullopt;
    }
    const auto goal_id = GoalIdFromKey(key);
    if (!goal_id.has_value()) {
      return std::nullopt;
    }
    return FeedbackSnapshot{.goal_id = *goal_id, .feedback = Hook::BuildFeedback(it->second)};
  }

  StatusSnapshot SnapshotStatusAndPrune(TimePoint now,
                                        std::chrono::seconds retention = std::chrono::seconds(60)) {
    StatusSnapshot snapshot;
    const auto ledger_snapshot = ledger_.SnapshotStatusAndPrune(now, retention);
    snapshot.pruned_keys = ledger_snapshot.pruned_keys;
    for (const auto& key : snapshot.pruned_keys) {
      pending_inputs_.erase(key);
      states_.erase(key);
    }
    for (const auto& entry : ledger_snapshot.status_list) {
      snapshot.status_list.push_back(
          StatusEntry{.goal_id = entry.goal_id, .stamp = entry.stamp, .status = entry.status});
    }
    return snapshot;
  }

  void RequestStopAll() { ledger_.RequestStopAll(); }

  bool HasGoal(const UuidT& goal_id) const { return ledger_.HasGoal(goal_id); }

 private:
  void FinalizeTerminal(ExecutionTransition& transition, const std::string& key,
                        int8_t terminal_status, const ExecState& state, TimePoint now) {
    const auto terminal = ledger_.MarkTerminal(key, terminal_status, now);
    transition.terminal = terminal.found;
    transition.final_status = terminal.final_status;
    transition.final_result = Hook::BuildResult(state);
    transition.pending_results = std::move(terminal.pending_results);
  }

  ResultPayload ResultForKey(const std::string& key) const {
    auto it = states_.find(key);
    if (it == states_.end()) {
      return ResultPayload{};
    }
    return Hook::BuildResult(it->second);
  }

  std::optional<UuidT> GoalIdFromKey(const std::string& key) const {
    const auto goal_info = ledger_.GoalInfoForKey(key);
    if (!goal_info.has_value()) {
      return std::nullopt;
    }
    return goal_info->goal_id;
  }

  Ledger ledger_;
  std::unordered_map<std::string, GoalInput> pending_inputs_;
  std::unordered_map<std::string, ExecState> states_;
};

}  // namespace ibmw::action

#endif  // IBMW_API_CPP_ACTION_SERVER_RUNTIME_H_
