// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) turnkey server node.
//
// ibmw::action::ActionServerNode<Traits, Hook, Bind> is the turnkey assembly of
// the three thinner public action layers: it owns the FastDDS participant, the
// three ROS 2 action services (send_goal / get_result / cancel_goal), the two
// publishers (feedback / status), the per-goal worker threads and the status
// loop, and drives them all through a generic ibmw::action::ServerRuntime<Hook>.
// Everything in this node is action-independent except the three wire-conversion
// sites, which are delegated to ActionServerBinding<Traits, Hook> (the Bind
// parameter). A complete DDS-only action server therefore reduces to:
//
//   1. an ActionTraits<MyAction> specialization (wire message typedefs),
//   2. a MyHook (goal validation + execution policy, no ROS 2 wire types),
//   3. an ActionServerBinding<ActionTraits<MyAction>, MyHook> specialization
//      (the three goal/result/feedback field conversions),
//   4. construct ActionServerNode<...>, call Start(domain_id), spin.
//
// The node preserves the exact lock scopes, publish ordering, terminal result
// fan-out, status loop cadence, prune behaviour and queue-full STATUS_UNKNOWN
// guard of the hand-written Fibonacci proof server it generalizes.

#ifndef IBMW_API_CPP_ACTION_ACTION_SERVER_NODE_H_
#define IBMW_API_CPP_ACTION_ACTION_SERVER_NODE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <action_msgs/msg/goal_status.hpp>
#include <action_msgs/msg/goal_status_array.hpp>
#include <action_msgs/srv/cancel_goal.hpp>
#include <builtin_interfaces/msg/time.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "cpp/action/action_server_binding.h"
#include "cpp/action/server.h"
#include "cpp/action/server_runtime.h"
#include "internal/ros2_action_dds/action_wire_contract.h"

namespace ibmw::action {

namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::internal::ros2_action_dds::rtps_compat;

// Turnkey DDS-only action server. Traits supplies the ROS 2 wire message types,
// Hook supplies the action's execution policy, and Bind (defaulting to
// ActionServerBinding<Traits, Hook>) supplies the three wire conversions.
template <typename Traits, typename Hook,
          typename Bind = ActionServerBinding<Traits, Hook>>
class ActionServerNode {
  static_assert(kActionServerBindingSpecialized<Traits, Hook>,
                "ActionServerNode requires an ActionServerBinding<Traits, Hook> "
                "specialization that sets kActionServerBindingSpecialized<Traits, "
                "Hook> = true; provide GoalInputOf / ToGetResultResponse / "
                "ToFeedbackMessage for this action.");

 public:
  using GoalId = typename Traits::GoalId;
  using SendGoalRequest = typename Traits::SendGoalRequest;
  using SendGoalResponse = typename Traits::SendGoalResponse;
  using GetResultRequest = typename Traits::GetResultRequest;
  using GetResultResponse = typename Traits::GetResultResponse;
  using FeedbackMessage = typename Traits::FeedbackMessage;
  using ResultPayload = typename Hook::ResultPayload;
  using FeedbackPayload = typename Hook::FeedbackPayload;

  explicit ActionServerNode(std::string action_name) : action_name_(std::move(action_name)) {}
  ~ActionServerNode() { Stop(); }

  ActionServerNode(const ActionServerNode&) = delete;
  ActionServerNode& operator=(const ActionServerNode&) = delete;

  bool Start(uint32_t domain_id) {
    dds::DomainParticipantQos pqos;
    pqos.name("ibmw_action_dds_server");
    participant_ = dds::DomainParticipantFactory::get_instance()->create_participant(domain_id, pqos);
    if (participant_ == nullptr) {
      std::cerr << "[FAIL] create_participant failed" << std::endl;
      return false;
    }
    publisher_ = participant_->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
    subscriber_ = participant_->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
    if (publisher_ == nullptr || subscriber_ == nullptr) {
      std::cerr << "[FAIL] create publisher/subscriber failed" << std::endl;
      return false;
    }

    const auto endpoint_names =
        ibmw::internal::ros2_action_dds::MakeEndpointNames(action_name_);
    if (!CreateService<SendGoalRequest, SendGoalResponse>(
            endpoint_names.send_goal_request, endpoint_names.send_goal_reply, send_goal_,
            &ActionServerNode::OnSendGoal)) {
      return false;
    }
    if (!CreateService<GetResultRequest, GetResultResponse>(
            endpoint_names.get_result_request, endpoint_names.get_result_reply, get_result_,
            &ActionServerNode::OnGetResult)) {
      return false;
    }
    if (!CreateService<action_msgs::srv::CancelGoal::Request, action_msgs::srv::CancelGoal::Response>(
            endpoint_names.cancel_goal_request, endpoint_names.cancel_goal_reply, cancel_goal_,
            &ActionServerNode::OnCancelGoal)) {
      return false;
    }
    if (!CreatePublisher<FeedbackMessage>(
            endpoint_names.feedback, ibmw::internal::ros2_action_dds::FeedbackQosProfile(),
            feedback_topic_, feedback_writer_)) {
      return false;
    }
    if (!CreatePublisher<action_msgs::msg::GoalStatusArray>(
            endpoint_names.status, ibmw::internal::ros2_action_dds::StatusQosProfile(),
            status_topic_, status_writer_)) {
      return false;
    }

    status_thread_ = std::thread([this] { StatusLoop(); });
    std::cout << "[PASS] DDS-only action server started for " << action_name_ << std::endl;
    return true;
  }

  void Stop() {
    stopping_.store(true);
    status_cv_.notify_all();
    if (status_thread_.joinable()) {
      status_thread_.join();
    }
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      runtime_.RequestStopAll();
      for (auto& [_, worker] : workers_) {
        if (worker.joinable()) {
          workers.push_back(std::move(worker));
        }
      }
      workers_.clear();
    }
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }

    if (participant_ != nullptr) {
      dds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
      participant_ = nullptr;
      publisher_ = nullptr;
      subscriber_ = nullptr;
    }
  }

  void OnSendGoal(const SendGoalRequest& request, const rtps::SampleIdentity& identity) {
    SendGoalResponse response;
    response.accepted = false;
    response.stamp = NowTime();
    bool publish_status = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto decision =
          runtime_.AcceptGoal(request.goal_id, response.stamp, Bind::GoalInputOf(request));
      if (decision.accepted) {
        workers_[decision.key] = std::thread([this, key = decision.key] { ExecuteGoal(key); });
        response.accepted = true;
        publish_status = true;
      } else if (decision.failure == GoalAcceptanceFailure::kInvalidGoal) {
        std::cerr << "[WARN] Rejecting goal: validation failed" << std::endl;
        publish_status = true;
      } else if (decision.failure == GoalAcceptanceFailure::kDuplicateGoal) {
        std::cerr << "[WARN] Rejecting duplicate goal id" << std::endl;
      } else if (decision.failure == GoalAcceptanceFailure::kCapacityFull) {
        std::cerr << "[WARN] Rejecting goal because server capacity is full" << std::endl;
      }
    }

    WriteResponse(send_goal_.writer, response, identity);
    if (publish_status) {
      PublishStatus();
    }
  }

  void OnGetResult(const GetResultRequest& request, const rtps::SampleIdentity& identity) {
    std::optional<GetResultResponse> response;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto decision = runtime_.GetResult(request.goal_id, PendingResult{identity});
      if (decision.immediate) {
        if (decision.queue_full) {
          std::cerr << "[WARN] GetResult pending queue is full; returning STATUS_UNKNOWN" << std::endl;
        }
        response = Bind::ToGetResultResponse(decision.status, decision.result);
      }
    }
    if (response.has_value()) {
      WriteResponse(get_result_.writer, *response, identity);
    }
  }

  void OnCancelGoal(const action_msgs::srv::CancelGoal::Request& request,
                    const rtps::SampleIdentity& identity) {
    action_msgs::srv::CancelGoal::Response response;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto decision = runtime_.CancelGoal(request.goal_info.goal_id, request.goal_info.stamp);
      response.return_code = decision.return_code;
      for (const auto& goal : decision.goals_canceling) {
        action_msgs::msg::GoalInfo info;
        info.goal_id = goal.goal_id;
        info.stamp = goal.stamp;
        response.goals_canceling.push_back(info);
      }
    }

    WriteResponse(cancel_goal_.writer, response, identity);
    PublishStatus();
  }

 private:
  using Clock = std::chrono::steady_clock;
  using SystemClock = std::chrono::system_clock;

  struct PendingResult {
    rtps::SampleIdentity identity;
  };

  using Runtime = ServerRuntime<Hook, GoalId, builtin_interfaces::msg::Time, PendingResult, Clock>;

  template <typename ReqT, typename RspT>
  using ServiceEndpoint = server::ServiceEndpoint<ActionServerNode, ReqT, RspT>;

  static builtin_interfaces::msg::Time NowTime() {
    const auto now = SystemClock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    builtin_interfaces::msg::Time stamp;
    stamp.sec = static_cast<int32_t>(seconds.count());
    stamp.nanosec = static_cast<uint32_t>(nanoseconds.count());
    return stamp;
  }

  template <typename ReqT, typename RspT>
  bool CreateService(const std::string& req_topic_name, const std::string& rsp_topic_name,
                     ServiceEndpoint<ReqT, RspT>& endpoint,
                     void (ActionServerNode::*handler)(const ReqT&, const rtps::SampleIdentity&)) {
    return server::CreateService<ActionServerNode, ReqT, RspT>(
        participant_, publisher_, subscriber_, req_topic_name, rsp_topic_name, endpoint, *this,
        handler);
  }

  template <typename MsgT>
  bool CreatePublisher(const std::string& topic_name,
                       const ibmw::internal::ros2_action_dds::EndpointQosProfile& qos_profile,
                       dds::Topic*& topic, dds::DataWriter*& writer) {
    return server::CreatePublisher<MsgT>(participant_, publisher_, topic_name, qos_profile,
                                         owned_types_, topic, writer);
  }

  template <typename RspT>
  void WriteResponse(dds::DataWriter* writer, RspT& response, const rtps::SampleIdentity& identity) {
    server::WriteResponse(writer, response, identity);
  }

  void ExecuteGoal(const std::string key) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runtime_.StartExecuting(key)) {
        return;
      }
    }
    PublishStatus();
    PublishFeedback(key);

    while (!stopping_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::vector<PendingResult> pending;
      ResultPayload final_result{};
      int8_t final_status = action_msgs::msg::GoalStatus::STATUS_UNKNOWN;
      bool terminal = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto transition = runtime_.AdvanceExecution(key, Clock::now());
        if (!transition.found) {
          return;
        }
        if (transition.terminal) {
          terminal = true;
          pending = std::move(transition.pending_results);
          final_result = transition.final_result;
          final_status = transition.final_status;
        }
      }
      PublishFeedback(key);
      PublishStatus();
      if (terminal) {
        auto response = Bind::ToGetResultResponse(final_status, final_result);
        for (const auto& pending_result : pending) {
          WriteResponse(get_result_.writer, response, pending_result.identity);
        }
        return;
      }
    }
  }

  void PublishFeedback(const std::string& key) {
    FeedbackMessage message;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto snapshot = runtime_.SnapshotFeedback(key);
      if (!snapshot.has_value()) {
        return;
      }
      message = Bind::ToFeedbackMessage(snapshot->goal_id, snapshot->feedback);
    }
    feedback_writer_->write(&message);
  }

  void PublishStatus() {
    action_msgs::msg::GoalStatusArray array;
    const auto now = Clock::now();
    std::vector<std::thread> retained_workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto snapshot = runtime_.SnapshotStatusAndPrune(now, std::chrono::seconds(60));
      for (const auto& key : snapshot.pruned_keys) {
        auto worker_it = workers_.find(key);
        if (worker_it != workers_.end()) {
          if (worker_it->second.joinable()) {
            retained_workers.push_back(std::move(worker_it->second));
          }
          workers_.erase(worker_it);
        }
      }
      for (const auto& entry : snapshot.status_list) {
        action_msgs::msg::GoalStatus status;
        status.goal_info.goal_id = entry.goal_id;
        status.goal_info.stamp = entry.stamp;
        status.status = entry.status;
        array.status_list.push_back(status);
      }
    }
    for (auto& worker : retained_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    status_writer_->write(&array);
    status_cv_.notify_all();
  }

  void StatusLoop() {
    while (!stopping_.load()) {
      PublishStatus();
      std::unique_lock<std::mutex> lock(status_mutex_);
      status_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stopping_.load(); });
    }
  }

  std::string action_name_;
  dds::DomainParticipant* participant_ = nullptr;
  dds::Publisher* publisher_ = nullptr;
  dds::Subscriber* subscriber_ = nullptr;
  ServiceEndpoint<SendGoalRequest, SendGoalResponse> send_goal_;
  ServiceEndpoint<GetResultRequest, GetResultResponse> get_result_;
  ServiceEndpoint<action_msgs::srv::CancelGoal::Request, action_msgs::srv::CancelGoal::Response>
      cancel_goal_;
  std::vector<dds::TypeSupport> owned_types_;
  dds::Topic* feedback_topic_ = nullptr;
  dds::Topic* status_topic_ = nullptr;
  dds::DataWriter* feedback_writer_ = nullptr;
  dds::DataWriter* status_writer_ = nullptr;
  std::mutex mutex_;
  Runtime runtime_;
  std::unordered_map<std::string, std::thread> workers_;
  std::atomic_bool stopping_{false};
  std::thread status_thread_;
  std::mutex status_mutex_;
  std::condition_variable status_cv_;
};

}  // namespace ibmw::action

#endif  // IBMW_API_CPP_ACTION_ACTION_SERVER_NODE_H_
