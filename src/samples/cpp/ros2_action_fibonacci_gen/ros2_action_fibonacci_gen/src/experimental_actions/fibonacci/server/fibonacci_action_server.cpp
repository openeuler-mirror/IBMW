// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY
#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <action_msgs/msg/detail/goal_status_array__rosidl_typesupport_fastrtps_cpp.hpp>
#include <action_msgs/msg/goal_status.hpp>
#include <action_msgs/msg/goal_status_array.hpp>
#include <action_msgs/srv/cancel_goal.hpp>
#include <action_msgs/srv/detail/cancel_goal__rosidl_typesupport_fastrtps_cpp.hpp>
#include <builtin_interfaces/msg/time.hpp>
#include <example_interfaces/action/detail/fibonacci__rosidl_typesupport_fastrtps_cpp.hpp>
#include <example_interfaces/action/fibonacci.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#if __has_include(<fastdds/rtps/common/SampleIdentity.hpp>)
#include <fastdds/rtps/common/SampleIdentity.hpp>
#else
#include <fastdds/rtps/common/SampleIdentity.h>
#endif
#include "cpp/action/server.h"
#include "../common/fibonacci_action_common.h"
#include "../common/fibonacci_action_fastdds_helpers.h"
#include "../common/fibonacci_action_fastdds_typesupport.h"
#include "../common/fibonacci_action_traits.h"
#include "../common/fibonacci_action_server_runtime.h"

namespace {

namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::internal::ros2_action_dds::rtps_compat;
namespace action_server = ibmw::action::server;
using Clock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using Fibonacci = example_interfaces::action::Fibonacci;
using Traits = ibmw::action::ActionTraits<Fibonacci>;
namespace action_common = ibmw::samples::ros2_action_fibonacci;

template <typename T>
using UuidMap = std::unordered_map<std::string, T>;

builtin_interfaces::msg::Time NowTime() {
  const auto now = SystemClock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
  builtin_interfaces::msg::Time stamp;
  stamp.sec = static_cast<int32_t>(seconds.count());
  stamp.nanosec = static_cast<uint32_t>(nanoseconds.count());
  return stamp;
}

class FibonacciActionServer;

template <typename ReqT>
using ServiceListener = action_server::ServiceListener<FibonacciActionServer, ReqT>;

struct PendingResult {
  rtps::SampleIdentity identity;
};

using Runtime = action_common::FibonacciActionServerRuntime<unique_identifier_msgs::msg::UUID,
                                                            builtin_interfaces::msg::Time,
                                                            PendingResult,
                                                            Clock>;

class FibonacciActionServer {
 public:
  FibonacciActionServer() = default;
  ~FibonacciActionServer() { Stop(); }

  bool Start(uint32_t domain_id) {
    dds::DomainParticipantQos pqos;
    pqos.name("ibmw_fibonacci_action_dds_server");
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

    const auto endpoint_names = action_common::MakeEndpointNames("/fibonacci");
    if (!CreateService<Traits::SendGoalRequest, Traits::SendGoalResponse>(
            endpoint_names.send_goal_request, endpoint_names.send_goal_reply, send_goal_,
            &FibonacciActionServer::OnSendGoal)) {
      return false;
    }
    if (!CreateService<Traits::GetResultRequest, Traits::GetResultResponse>(
            endpoint_names.get_result_request, endpoint_names.get_result_reply, get_result_,
            &FibonacciActionServer::OnGetResult)) {
      return false;
    }
    if (!CreateService<action_msgs::srv::CancelGoal::Request, action_msgs::srv::CancelGoal::Response>(
            endpoint_names.cancel_goal_request, endpoint_names.cancel_goal_reply, cancel_goal_,
            &FibonacciActionServer::OnCancelGoal)) {
      return false;
    }
    if (!CreatePublisher<Traits::FeedbackMessage>(endpoint_names.feedback,
                                                           action_common::FeedbackQosProfile(), feedback_topic_,
                                                           feedback_writer_)) {
      return false;
    }
    if (!CreatePublisher<action_msgs::msg::GoalStatusArray>(endpoint_names.status,
                                                            action_common::StatusQosProfile(), status_topic_,
                                                            status_writer_)) {
      return false;
    }

    status_thread_ = std::thread([this] { StatusLoop(); });
    std::cout << "[PASS] DDS-only Fibonacci action server started for /fibonacci" << std::endl;
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

  void OnSendGoal(const Traits::SendGoalRequest& request,
                  const rtps::SampleIdentity& identity) {
    Traits::SendGoalResponse response;
    response.accepted = false;
    response.stamp = NowTime();
    bool publish_status = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto decision = runtime_.AcceptGoal(request.goal_id, response.stamp, request.goal.order);
      if (decision.accepted) {
        workers_[decision.key] = std::thread([this, key = decision.key] { ExecuteGoal(key); });
        response.accepted = true;
        publish_status = true;
      } else if (decision.failure == action_common::GoalAcceptanceFailure::kInvalidOrder) {
        std::cerr << "[WARN] Rejecting goal with unsupported order=" << request.goal.order
                  << " (allowed range: 1.." << action_common::kMaxFibonacciOrder << ")" << std::endl;
        publish_status = true;
      } else if (decision.failure == action_common::GoalAcceptanceFailure::kDuplicateGoal) {
        std::cerr << "[WARN] Rejecting duplicate goal id" << std::endl;
      } else if (decision.failure == action_common::GoalAcceptanceFailure::kCapacityFull) {
        std::cerr << "[WARN] Rejecting goal because proof server capacity is full" << std::endl;
      }
    }

    WriteResponse(send_goal_.writer, response, identity);
    if (publish_status) {
      PublishStatus();
    }
  }

  void OnGetResult(const Traits::GetResultRequest& request,
                   const rtps::SampleIdentity& identity) {
    std::optional<Traits::GetResultResponse> response;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto decision = runtime_.GetResult(request.goal_id, PendingResult{identity});
      if (decision.immediate) {
        if (decision.queue_full) {
          std::cerr << "[WARN] GetResult pending queue is full; returning STATUS_UNKNOWN" << std::endl;
        }
        response = MakeResultResponse(decision.status, decision.sequence);
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
  template <typename ReqT, typename RspT>
  using ServiceEndpoint = action_server::ServiceEndpoint<FibonacciActionServer, ReqT, RspT>;

  template <typename ReqT, typename RspT>
  bool CreateService(const std::string& req_topic_name, const std::string& rsp_topic_name,
                     ServiceEndpoint<ReqT, RspT>& endpoint,
                     void (FibonacciActionServer::*handler)(const ReqT&, const rtps::SampleIdentity&)) {
    return action_server::CreateService<FibonacciActionServer, ReqT, RspT>(
        participant_, publisher_, subscriber_, req_topic_name, rsp_topic_name, endpoint, *this, handler);
  }

  template <typename MsgT>
  bool CreatePublisher(const std::string& topic_name, const action_common::EndpointQosProfile& qos_profile,
                       dds::Topic*& topic,
                       dds::DataWriter*& writer) {
    return action_server::CreatePublisher<MsgT>(participant_, publisher_, topic_name, qos_profile,
                                                owned_types_, topic, writer);
  }

  template <typename RspT>
  void WriteResponse(dds::DataWriter* writer, RspT& response, const rtps::SampleIdentity& identity) {
    action_server::WriteResponse(writer, response, identity);
  }

  static Traits::GetResultResponse MakeResultResponse(int8_t status,
                                                      const std::vector<int32_t>& sequence) {
    Traits::GetResultResponse response;
    response.status = status;
    response.result.sequence = sequence;
    return response;
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
      std::vector<int32_t> final_sequence;
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
          final_sequence = transition.final_sequence;
          final_status = transition.final_status;
        }
      }
      PublishFeedback(key);
      PublishStatus();
      if (terminal) {
        auto response = MakeResultResponse(final_status, final_sequence);
        for (const auto& pending_result : pending) {
          WriteResponse(get_result_.writer, response, pending_result.identity);
        }
        return;
      }
    }
  }

  void PublishFeedback(const std::string& key) {
    Traits::FeedbackMessage message;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto snapshot = runtime_.SnapshotFeedback(key);
      if (!snapshot.has_value()) {
        return;
      }
      message.goal_id = snapshot->goal_id;
      message.feedback.sequence = snapshot->sequence;
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

  dds::DomainParticipant* participant_ = nullptr;
  dds::Publisher* publisher_ = nullptr;
  dds::Subscriber* subscriber_ = nullptr;
  ServiceEndpoint<Traits::SendGoalRequest, Traits::SendGoalResponse> send_goal_;
  ServiceEndpoint<Traits::GetResultRequest, Traits::GetResultResponse> get_result_;
  ServiceEndpoint<action_msgs::srv::CancelGoal::Request, action_msgs::srv::CancelGoal::Response> cancel_goal_;
  std::vector<dds::TypeSupport> owned_types_;
  dds::Topic* feedback_topic_ = nullptr;
  dds::Topic* status_topic_ = nullptr;
  dds::DataWriter* feedback_writer_ = nullptr;
  dds::DataWriter* status_writer_ = nullptr;
  std::mutex mutex_;
  Runtime runtime_;
  UuidMap<std::thread> workers_;
  std::atomic_bool stopping_{false};
  std::thread status_thread_;
  std::mutex status_mutex_;
  std::condition_variable status_cv_;
};

std::optional<uint32_t> ParseDomainId(int argc, char** argv) {
  const char* flag = "--domain-id";
  const char* text = nullptr;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      text = argv[i + 1];
      break;
    }
  }
  if (text == nullptr) {
    flag = "ROS_DOMAIN_ID";
    text = std::getenv("ROS_DOMAIN_ID");
    if (text == nullptr || std::strlen(text) == 0U) {
      return 0U;
    }
  }
  uint32_t domain_id = 0;
  if (!action_common::ParseInteger<uint32_t>(text, 0, action_common::kMaxDomainId, domain_id)) {
    std::cerr << "[FAIL] Invalid value for " << flag << ": '" << text << "'" << std::endl;
    return std::nullopt;
  }
  return domain_id;
}

}  // namespace

int main(int argc, char** argv) {
  const auto domain_id = ParseDomainId(argc, argv);
  if (!domain_id.has_value()) {
    return 2;
  }
  // Block SIGINT/SIGTERM before Start() spawns any thread (FastDDS, status,
  // workers) so every thread inherits the mask, then take them synchronously
  // with sigwait() instead of polling a flag set by a handler.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGINT);
  sigaddset(&stop_signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
    std::cerr << "[FAIL] Cannot block SIGINT/SIGTERM" << std::endl;
    return 1;
  }
  FibonacciActionServer server;
  if (!server.Start(*domain_id)) {
    return 1;
  }
  int signal_number = 0;
  if (sigwait(&stop_signals, &signal_number) != 0) {
    std::cerr << "[FAIL] sigwait failed" << std::endl;
    return 1;
  }
  return 0;
}
