// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// INTERNAL ONLY: generic ROS 2 Action DDS goal lifecycle bookkeeping. This
// header is not a public API and must not be installed or included by
// generated, schema, template, or public API code.

#ifndef IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_GOAL_LEDGER_H_
#define IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_GOAL_LEDGER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::internal::ros2_action_dds {

enum class ActionGoalAcceptCode {
  kAccepted,
  kDuplicateGoal,
  kCapacityFull,
};

enum class ActionGetResultCode {
  kDeferred,
  kImmediateUnknown,
  kImmediateTerminal,
  kPendingQueueFull,
};

template <typename UuidT, typename TimeT, typename PendingResultT,
          typename ClockT = std::chrono::steady_clock>
class ActionGoalLedger {
 public:
  using TimePoint = typename ClockT::time_point;

  struct AcceptGoalDecision {
    bool accepted = false;
    ActionGoalAcceptCode code = ActionGoalAcceptCode::kAccepted;
    std::string key;
  };

  struct GetResultDecision {
    bool immediate = false;
    int8_t status = 0;
    ActionGetResultCode code = ActionGetResultCode::kDeferred;
  };

  struct GoalInfoEntry {
    UuidT goal_id;
    TimeT stamp;
  };

  struct CancelDecision {
    int8_t return_code = 0;
    std::vector<GoalInfoEntry> goals_canceling;
  };

  struct TerminalTransition {
    bool found = false;
    int8_t final_status = 0;
    std::vector<PendingResultT> pending_results;
  };

  struct StatusEntry {
    UuidT goal_id;
    TimeT stamp;
    int8_t status = 0;
  };

  struct StatusSnapshot {
    std::vector<StatusEntry> status_list;
    std::vector<std::string> pruned_keys;
  };

  AcceptGoalDecision AcceptGoal(const UuidT& goal_id,
                                const TimeT& stamp,
                                int8_t accepted_status,
                                std::size_t max_active,
                                std::size_t max_retained) {
    AcceptGoalDecision decision;
    decision.key = UuidToString(goal_id);
    if (goals_.find(decision.key) != goals_.end()) {
      decision.code = ActionGoalAcceptCode::kDuplicateGoal;
      return decision;
    }
    if (ActiveGoalCount() >= max_active || goals_.size() >= max_retained) {
      decision.code = ActionGoalAcceptCode::kCapacityFull;
      return decision;
    }

    GoalRecord goal;
    goal.goal_id = goal_id;
    goal.stamp = stamp;
    goal.status = accepted_status;
    goals_.emplace(decision.key, std::move(goal));
    decision.accepted = true;
    return decision;
  }

  bool SetExecuting(const std::string& key, int8_t executing_status) {
    auto it = goals_.find(key);
    if (it == goals_.end()) {
      return false;
    }
    it->second.status = executing_status;
    return true;
  }

  GetResultDecision GetResult(const UuidT& goal_id,
                              PendingResultT pending_result,
                              std::size_t max_pending_results_per_goal) {
    auto it = goals_.find(UuidToString(goal_id));
    if (it == goals_.end()) {
      return GetResultDecision{.immediate = true,
                               .status = 0,
                               .code = ActionGetResultCode::kImmediateUnknown};
    }
    if (it->second.terminal) {
      return GetResultDecision{.immediate = true,
                               .status = it->second.status,
                               .code = ActionGetResultCode::kImmediateTerminal};
    }
    if (it->second.pending_results.size() >= max_pending_results_per_goal) {
      return GetResultDecision{.immediate = true,
                               .status = 0,
                               .code = ActionGetResultCode::kPendingQueueFull};
    }
    it->second.pending_results.push_back(std::move(pending_result));
    return GetResultDecision{};
  }

  CancelDecision CancelGoal(const UuidT& goal_id,
                            const TimeT& stamp,
                            int8_t canceling_status,
                            int8_t return_none,
                            int8_t return_unknown_goal_id) {
    CancelDecision decision;
    decision.return_code = return_unknown_goal_id;
    const auto request_key = UuidToString(goal_id);
    const bool cancel_all_active = IsCancelAllActiveRequest(goal_id, stamp);
    for (auto& [key, goal] : goals_) {
      if (goal.terminal) {
        continue;
      }
      if (!cancel_all_active && key != request_key) {
        continue;
      }
      goal.cancel_requested = true;
      goal.status = canceling_status;
      decision.goals_canceling.push_back(GoalInfoEntry{.goal_id = goal.goal_id, .stamp = goal.stamp});
      decision.return_code = return_none;
    }
    if (decision.goals_canceling.empty() && cancel_all_active) {
      decision.return_code = return_none;
    }
    return decision;
  }

  TerminalTransition MarkTerminal(const std::string& key, int8_t terminal_status, TimePoint now) {
    TerminalTransition transition;
    auto it = goals_.find(key);
    if (it == goals_.end()) {
      return transition;
    }
    auto& goal = it->second;
    goal.status = terminal_status;
    goal.terminal = true;
    goal.terminal_time = now;
    transition.found = true;
    transition.final_status = goal.status;
    transition.pending_results = std::move(goal.pending_results);
    return transition;
  }

  StatusSnapshot SnapshotStatusAndPrune(TimePoint now, std::chrono::seconds retention) {
    StatusSnapshot snapshot;
    for (auto it = goals_.begin(); it != goals_.end();) {
      if (it->second.terminal && ResultRetentionExpired<ClockT>(it->second.terminal_time, now, retention)) {
        snapshot.pruned_keys.push_back(it->first);
        it = goals_.erase(it);
        continue;
      }
      snapshot.status_list.push_back(StatusEntry{.goal_id = it->second.goal_id,
                                                 .stamp = it->second.stamp,
                                                 .status = it->second.status});
      ++it;
    }
    return snapshot;
  }

  void RequestStopAll() {
    for (auto& [_, goal] : goals_) {
      goal.cancel_requested = true;
    }
  }

  bool HasGoal(const UuidT& goal_id) const { return goals_.find(UuidToString(goal_id)) != goals_.end(); }

  std::optional<GoalInfoEntry> GoalInfoForKey(const std::string& key) const {
    auto it = goals_.find(key);
    if (it == goals_.end()) {
      return std::nullopt;
    }
    return GoalInfoEntry{.goal_id = it->second.goal_id, .stamp = it->second.stamp};
  }

  bool IsCancelRequested(const std::string& key) const {
    auto it = goals_.find(key);
    return it != goals_.end() && it->second.cancel_requested;
  }

 private:
  struct GoalRecord {
    UuidT goal_id;
    TimeT stamp;
    int8_t status = 0;
    bool cancel_requested = false;
    bool terminal = false;
    std::vector<PendingResultT> pending_results;
    TimePoint terminal_time;
  };

  std::size_t ActiveGoalCount() const {
    std::size_t active = 0;
    for (const auto& [_, goal] : goals_) {
      if (!goal.terminal) {
        ++active;
      }
    }
    return active;
  }

  std::unordered_map<std::string, GoalRecord> goals_;
};

}  // namespace ibmw::internal::ros2_action_dds

#endif  // IBMW_INTERNAL_ROS2_ACTION_DDS_ACTION_GOAL_LEDGER_H_
