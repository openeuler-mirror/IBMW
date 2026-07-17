// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <action_msgs/msg/goal_status.hpp>
#include <action_msgs/msg/goal_status_array.hpp>
#include <action_msgs/msg/detail/goal_status_array__rosidl_typesupport_fastrtps_cpp.hpp>
#include <action_msgs/srv/cancel_goal.hpp>
#include <action_msgs/srv/detail/cancel_goal__rosidl_typesupport_fastrtps_cpp.hpp>
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
#include "cpp/action/client.h"
#include "../common/fibonacci_action_client_runtime.h"
#include "../common/fibonacci_action_common.h"
#include "../common/fibonacci_action_fastdds_helpers.h"
#include "../common/fibonacci_action_fastdds_typesupport.h"
#include "../common/fibonacci_action_traits.h"

namespace {

namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::internal::ros2_action_dds::rtps_compat;
namespace action_client = ibmw::action::client;
#if IBMW_ACTION_HAS_MODERN_FASTDDS
inline constexpr auto kReturnOk = eprosima::fastdds::dds::RETCODE_OK;
#else
inline constexpr auto kReturnOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
#endif
using Clock = std::chrono::steady_clock;
using Fibonacci = example_interfaces::action::Fibonacci;
using Traits = ibmw::action::ActionTraits<Fibonacci>;
namespace action_common = ibmw::samples::ros2_action_fibonacci;
using action_common::MakeTypeSupport;

template <typename RspT>
using ServiceCallResult = action_client::ServiceCallResult<RspT>;

template <typename ReqT, typename RspT>
using ServiceClientEndpoint = action_client::ServiceClientEndpoint<ReqT, RspT>;

struct Options {
  std::string mode = "success";
  int32_t order = 8;
  std::string action_name = "/fibonacci";
  uint32_t domain_id = 0;
  int server_wait_seconds = 10;
  int result_wait_seconds = 10;
  bool show_help = false;
};

struct FeedbackState {
  std::mutex mutex;
  action_common::FibonacciActionClientRuntime runtime;
};

struct StatusState {
  std::mutex mutex;
  action_common::FibonacciActionClientRuntime runtime;
};

class FeedbackListener final : public dds::DataReaderListener {
 public:
  explicit FeedbackListener(FeedbackState& state) : state_(state) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo sample_info;
    Traits::FeedbackMessage message;
    while (reader->take_next_sample(&message, &sample_info) == kReturnOk) {
      if (!sample_info.valid_data) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.runtime.ObserveFeedback(message.goal_id, message.feedback.sequence);
      }
      message = Traits::FeedbackMessage();
    }
  }

 private:
  FeedbackState& state_;
};

class StatusListener final : public dds::DataReaderListener {
 public:
  explicit StatusListener(StatusState& state) : state_(state) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo sample_info;
    action_msgs::msg::GoalStatusArray array;
    while (reader->take_next_sample(&array, &sample_info) == kReturnOk) {
      if (!sample_info.valid_data) {
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.runtime.ObserveStatusList(array.status_list);
      }
      array = action_msgs::msg::GoalStatusArray();
    }
  }

 private:
  StatusState& state_;
};

uint32_t ReadDefaultDomainId() {
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  if (domain == nullptr || std::strlen(domain) == 0U) {
    return 0;
  }
  return static_cast<uint32_t>(std::stoul(domain));
}

void PrintUsage() {
  std::cout
      << "Usage: fibonacci_action_dds_client [options]\n"
      << "  --mode <success|cancel>          Validation mode (default: success)\n"
      << "  --order <n>                      Fibonacci goal order (default: 8)\n"
      << "  --action-name <name>             Action name (default: /fibonacci)\n"
      << "  --domain-id <id>                 DDS domain id (default: ROS_DOMAIN_ID or 0)\n"
      << "  --server-wait-seconds <seconds>  Endpoint match wait (default: 10)\n"
      << "  --result-wait-seconds <seconds>  Result wait timeout (default: 10)\n"
      << "  --help                           Show this message\n";
}

std::optional<Options> ParseArgs(int argc, char* argv[]) {
  Options options;
  options.domain_id = ReadDefaultDomainId();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "[FAIL] Missing value for " << flag << std::endl;
        return nullptr;
      }
      ++i;
      return argv[i];
    };

    if (arg == "--mode") {
      const char* value = require_value("--mode");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.mode = value;
    } else if (arg == "--order") {
      const char* value = require_value("--order");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.order = std::stoi(value);
    } else if (arg == "--action-name") {
      const char* value = require_value("--action-name");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.action_name = value;
    } else if (arg == "--domain-id") {
      const char* value = require_value("--domain-id");
      if (value == nullptr) {
        return std::nullopt;
      }
      options.domain_id = static_cast<uint32_t>(std::stoul(value));
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
      options.show_help = true;
      PrintUsage();
      return options;
    } else {
      std::cerr << "[FAIL] Unknown argument: " << arg << std::endl;
      return std::nullopt;
    }
  }

  if (options.mode != "success" && options.mode != "cancel") {
    std::cerr << "[FAIL] --mode must be success or cancel" << std::endl;
    return std::nullopt;
  }
  if (!action_common::IsValidGoalOrder(options.order)) {
    std::cerr << "[FAIL] --order must be in range 1.." << action_common::kMaxFibonacciOrder << std::endl;
    return std::nullopt;
  }
  if (options.server_wait_seconds < 0 || options.result_wait_seconds < 0) {
    std::cerr << "[FAIL] wait timeouts must be >= 0" << std::endl;
    return std::nullopt;
  }
  return options;
}

unique_identifier_msgs::msg::UUID MakeGoalId() {
  unique_identifier_msgs::msg::UUID uuid;
  const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
  for (std::size_t i = 0; i < uuid.uuid.size(); ++i) {
    uuid.uuid[i] = static_cast<uint8_t>((now >> ((i % 8U) * 8U)) ^ (0xA5U + i));
  }
  return uuid;
}

bool SequenceMatches(const std::vector<int32_t>& actual, const std::vector<int32_t>& expected) {
  return actual == expected;
}

class FibonacciActionClient {
 public:
  explicit FibonacciActionClient(Options options) : options_(std::move(options)) {}
  ~FibonacciActionClient() { Stop(); }

  bool Start() {
    dds::DomainParticipantQos pqos;
    pqos.name("ibmw_fibonacci_action_dds_client");
    participant_ = dds::DomainParticipantFactory::get_instance()->create_participant(options_.domain_id, pqos);
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

    const auto endpoint_names = action_common::MakeEndpointNames(options_.action_name);
    if (!send_goal_.Create(participant_, publisher_, subscriber_, endpoint_names.send_goal_request,
                           endpoint_names.send_goal_reply)) {
      return false;
    }
    if (!get_result_.Create(participant_, publisher_, subscriber_, endpoint_names.get_result_request,
                            endpoint_names.get_result_reply)) {
      return false;
    }
    if (!cancel_goal_.Create(participant_, publisher_, subscriber_, endpoint_names.cancel_goal_request,
                             endpoint_names.cancel_goal_reply)) {
      return false;
    }
    if (!CreateFeedbackReader(endpoint_names.feedback)) {
      return false;
    }
    return CreateStatusReader(endpoint_names.status);
  }

  int RunSuccess() {
    if (!WaitForServer()) {
      return 1;
    }

    const uint32_t send_goal_sequence = 1;
    const auto goal_id = MakeGoalId();
    ResetObservationForGoal(goal_id);

    Traits::SendGoalRequest send_goal_request;
    send_goal_request.goal_id = goal_id;
    send_goal_request.goal.order = options_.order;
    const auto send_goal_call = CallService(send_goal_, send_goal_request, send_goal_sequence,
                                            std::chrono::seconds(options_.server_wait_seconds),
                                            "[FAIL] send_goal write failed");
    if (!send_goal_call.write_ok) {
      return 1;
    }
    const auto& send_goal_response = send_goal_call.response;
    if (!send_goal_response.has_value()) {
      std::cerr << "[FAIL] timed out waiting for send_goal response" << std::endl;
      return 1;
    }
    if (!send_goal_response->accepted) {
      std::cerr << "[FAIL] send_goal response was rejected" << std::endl;
      return 1;
    }

    const uint32_t get_result_sequence = 1;
    Traits::GetResultRequest result_request;
    result_request.goal_id = goal_id;
    const auto result_call = CallService(get_result_, result_request, get_result_sequence,
                                         std::chrono::seconds(options_.result_wait_seconds),
                                         "[FAIL] get_result write failed");
    if (!result_call.write_ok) {
      return 1;
    }
    const auto& result_response = result_call.response;
    if (!result_response.has_value()) {
      std::cerr << "[FAIL] timed out waiting for get_result response" << std::endl;
      return 1;
    }
    if (result_response->status != action_msgs::msg::GoalStatus::STATUS_SUCCEEDED) {
      std::cerr << "[FAIL] expected succeeded status, got " << static_cast<int>(result_response->status) << std::endl;
      return 1;
    }

    const std::vector<int32_t> expected = action_common::BuildFibonacciSequence(options_.order);
    if (!SequenceMatches(result_response->result.sequence, expected)) {
      std::cerr << "[FAIL] expected result " << action_common::JoinSequence(expected)
                << ", got " << action_common::JoinSequence(result_response->result.sequence) << std::endl;
      return 1;
    }

    std::cout << "[INFO] Goal accepted" << std::endl;
    std::cout << "[INFO] Result sequence=" << action_common::JoinSequence(result_response->result.sequence)
              << std::endl;
    if (!ValidateFeedbackIfAvailable(expected)) {
      return 1;
    }
    ObserveStatusIfAvailable(result_response->status);
    std::cout << "[PASS] Success case matched deterministic Fibonacci result" << std::endl;
    return 0;
  }

  int RunCancel() {
    if (!WaitForServer()) {
      return 1;
    }

    const uint32_t send_goal_sequence = 1;
    const auto goal_id = MakeGoalId();
    ResetObservationForGoal(goal_id);

    Traits::SendGoalRequest send_goal_request;
    send_goal_request.goal_id = goal_id;
    send_goal_request.goal.order = options_.order;
    const auto send_goal_call = CallService(send_goal_, send_goal_request, send_goal_sequence,
                                            std::chrono::seconds(options_.server_wait_seconds),
                                            "[FAIL] send_goal write failed");
    if (!send_goal_call.write_ok) {
      return 1;
    }
    const auto& send_goal_response = send_goal_call.response;
    if (!send_goal_response.has_value()) {
      std::cerr << "[FAIL] timed out waiting for send_goal response" << std::endl;
      return 1;
    }
    if (!send_goal_response->accepted) {
      std::cerr << "[FAIL] send_goal response was rejected" << std::endl;
      return 1;
    }

    const uint32_t cancel_sequence = 1;
    action_msgs::srv::CancelGoal::Request cancel_request;
    cancel_request.goal_info.goal_id = goal_id;
    cancel_request.goal_info.stamp.sec = 0;
    cancel_request.goal_info.stamp.nanosec = 0;
    const auto cancel_call = CallService(cancel_goal_, cancel_request, cancel_sequence,
                                         std::chrono::seconds(options_.server_wait_seconds),
                                         "[FAIL] cancel_goal write failed");
    if (!cancel_call.write_ok) {
      return 1;
    }
    const auto& cancel_response = cancel_call.response;
    if (!cancel_response.has_value()) {
      std::cerr << "[FAIL] timed out waiting for cancel_goal response" << std::endl;
      return 1;
    }
    if (cancel_response->return_code != action_msgs::srv::CancelGoal::Response::ERROR_NONE) {
      std::cerr << "[FAIL] expected cancel ERROR_NONE, got "
                << static_cast<int>(cancel_response->return_code) << std::endl;
      return 1;
    }
    const auto matching_goal = std::find_if(
        cancel_response->goals_canceling.begin(), cancel_response->goals_canceling.end(),
        [&goal_id](const action_msgs::msg::GoalInfo& info) { return info.goal_id.uuid == goal_id.uuid; });
    if (matching_goal == cancel_response->goals_canceling.end()) {
      std::cerr << "[FAIL] cancel response did not include the submitted goal id" << std::endl;
      return 1;
    }

    const uint32_t get_result_sequence = 1;
    Traits::GetResultRequest result_request;
    result_request.goal_id = goal_id;
    const auto result_call = CallService(get_result_, result_request, get_result_sequence,
                                         std::chrono::seconds(options_.result_wait_seconds),
                                         "[FAIL] get_result write failed");
    if (!result_call.write_ok) {
      return 1;
    }
    const auto& result_response = result_call.response;
    if (!result_response.has_value()) {
      std::cerr << "[FAIL] timed out waiting for get_result response" << std::endl;
      return 1;
    }
    if (result_response->status != action_msgs::msg::GoalStatus::STATUS_CANCELED) {
      std::cerr << "[FAIL] expected canceled status, got " << static_cast<int>(result_response->status) << std::endl;
      return 1;
    }

    std::cout << "[INFO] Goal accepted" << std::endl;
    std::cout << "[INFO] Cancel response return_code=" << static_cast<int>(cancel_response->return_code)
              << " goals_canceling=" << cancel_response->goals_canceling.size() << std::endl;
    std::cout << "[INFO] Cancel matched submitted goal id" << std::endl;
    std::cout << "[INFO] Result status=" << static_cast<int>(result_response->status) << std::endl;
    std::cout << "[INFO] Result sequence=" << action_common::JoinSequence(result_response->result.sequence)
              << std::endl;
    if (!ValidateFeedbackIfAvailable(result_response->result.sequence)) {
      return 1;
    }
    ObserveStatusIfAvailable(result_response->status);
    std::cout << "[PASS] Cancel case matched deterministic Fibonacci cancellation" << std::endl;
    return 0;
  }

 private:
  template <typename ReqT, typename RspT>
  ServiceCallResult<RspT> CallService(ServiceClientEndpoint<ReqT, RspT>& endpoint, ReqT& request,
                                      uint32_t sequence, std::chrono::seconds timeout,
                                      const char* write_failure_message) {
    return action_client::CallService(endpoint, request, sequence, timeout, write_failure_message);
  }

  bool WaitForServer() const {
    const auto timeout = std::chrono::seconds(options_.server_wait_seconds);
    if (!send_goal_.WaitForServer(timeout)) {
      std::cerr << "[FAIL] timed out waiting for send_goal endpoints" << std::endl;
      return false;
    }
    if (!get_result_.WaitForServer(timeout)) {
      std::cerr << "[FAIL] timed out waiting for get_result endpoints" << std::endl;
      return false;
    }
    if (!cancel_goal_.WaitForServer(timeout)) {
      std::cerr << "[FAIL] timed out waiting for cancel_goal endpoints" << std::endl;
      return false;
    }
    if (!WaitForStatusEndpoint(timeout)) {
      std::cerr << "[WARN] timed out waiting for status endpoint; continuing without status observations" << std::endl;
    }
    return true;
  }

  bool CreateFeedbackReader(const std::string& topic_name) {
    feedback_ts_ = MakeTypeSupport<Traits::FeedbackMessage>();
    feedback_ts_.register_type(participant_);
    feedback_topic_ = participant_->create_topic(topic_name, feedback_ts_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
    if (feedback_topic_ == nullptr) {
      std::cerr << "[FAIL] create feedback topic failed: " << topic_name << std::endl;
      return false;
    }
    dds::DataReaderQos qos;
    action_common::ApplyFeedbackEndpointQos(qos);
    feedback_reader_ = subscriber_->create_datareader(feedback_topic_, qos);
    if (feedback_reader_ == nullptr) {
      std::cerr << "[FAIL] create feedback reader failed: " << topic_name << std::endl;
      return false;
    }
    return true;
  }

  bool CreateStatusReader(const std::string& topic_name) {
    status_ts_ = MakeTypeSupport<action_msgs::msg::GoalStatusArray>();
    status_ts_.register_type(participant_);
    status_topic_ = participant_->create_topic(topic_name, status_ts_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
    if (status_topic_ == nullptr) {
      std::cerr << "[FAIL] create status topic failed: " << topic_name << std::endl;
      return false;
    }
    dds::DataReaderQos qos;
    action_common::ApplyStatusEndpointQos(qos);
    status_reader_ = subscriber_->create_datareader(status_topic_, qos);
    if (status_reader_ == nullptr) {
      std::cerr << "[FAIL] create status reader failed: " << topic_name << std::endl;
      return false;
    }
    return true;
  }

  bool WaitForStatusEndpoint(std::chrono::seconds timeout) const {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() <= deadline) {
      dds::SubscriptionMatchedStatus subscription_status;
      const bool subscription_ok = status_reader_->get_subscription_matched_status(subscription_status) ==
                                   kReturnOk;
      if (subscription_ok && subscription_status.current_count > 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  bool ValidateFeedbackIfAvailable(const std::vector<int32_t>& expected) const {
    std::lock_guard<std::mutex> lock(feedback_state_.mutex);
    const auto validation = feedback_state_.runtime.ValidateFeedbackPrefix(expected);
    if (!validation.available) {
      return true;
    }
    if (!validation.ok) {
      std::cerr << "[FAIL] feedback for goal did not match expected Fibonacci prefix: "
                << action_common::JoinSequence(validation.sequence) << std::endl;
      return false;
    }
    std::cout << "[INFO] Feedback sequence=" << action_common::JoinSequence(validation.sequence)
              << std::endl;
    return true;
  }

  void DetachObservationListeners() {
    if (feedback_reader_ != nullptr) {
      feedback_reader_->set_listener(nullptr);
    }
    feedback_listener_.reset();
    if (status_reader_ != nullptr) {
      status_reader_->set_listener(nullptr);
    }
    status_listener_.reset();
  }

  void ResetObservationForGoal(const unique_identifier_msgs::msg::UUID& goal_id) {
    DetachObservationListeners();
    {
      std::lock_guard<std::mutex> lock(feedback_state_.mutex);
      feedback_state_.runtime.ResetActiveGoal(goal_id);
    }
    {
      std::lock_guard<std::mutex> lock(status_state_.mutex);
      status_state_.runtime.ResetActiveGoal(goal_id);
    }
    feedback_listener_ = std::make_unique<FeedbackListener>(feedback_state_);
    feedback_reader_->set_listener(feedback_listener_.get());
    status_listener_ = std::make_unique<StatusListener>(status_state_);
    status_reader_->set_listener(status_listener_.get());
  }

  void ObserveStatusIfAvailable(int8_t terminal_status) const {
    std::lock_guard<std::mutex> lock(status_state_.mutex);
    const auto summary = status_state_.runtime.SummarizeTerminalStatus(terminal_status);
    if (!summary.available) {
      return;
    }
    std::cout << "[INFO] Status arrays observed=" << summary.observed_arrays
              << " matched_statuses=" << summary.matched_statuses
              << " saw_terminal=" << (summary.saw_terminal ? "true" : "false")
              << " latest_status=" << static_cast<int>(summary.latest_status) << std::endl;
  }

  void Stop() {
    if (participant_ != nullptr) {
      send_goal_.DetachListener();
      get_result_.DetachListener();
      cancel_goal_.DetachListener();
      DetachObservationListeners();
      dds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
      participant_ = nullptr;
      publisher_ = nullptr;
      subscriber_ = nullptr;
      feedback_reader_ = nullptr;
      status_reader_ = nullptr;
    }
  }

  Options options_;
  dds::DomainParticipant* participant_ = nullptr;
  dds::Publisher* publisher_ = nullptr;
  dds::Subscriber* subscriber_ = nullptr;
  ServiceClientEndpoint<Traits::SendGoalRequest, Traits::SendGoalResponse> send_goal_;
  ServiceClientEndpoint<Traits::GetResultRequest, Traits::GetResultResponse> get_result_;
  ServiceClientEndpoint<action_msgs::srv::CancelGoal::Request, action_msgs::srv::CancelGoal::Response> cancel_goal_;
  dds::TypeSupport feedback_ts_;
  dds::TypeSupport status_ts_;
  dds::Topic* feedback_topic_ = nullptr;
  dds::Topic* status_topic_ = nullptr;
  dds::DataReader* feedback_reader_ = nullptr;
  dds::DataReader* status_reader_ = nullptr;
  mutable FeedbackState feedback_state_;
  mutable StatusState status_state_;
  std::unique_ptr<FeedbackListener> feedback_listener_;
  std::unique_ptr<StatusListener> status_listener_;
};

}  // namespace

int main(int argc, char* argv[]) {
  const auto options = ParseArgs(argc, argv);
  if (!options.has_value()) {
    return 2;
  }
  if (options->show_help) {
    return 0;
  }
  FibonacciActionClient client(*options);
  if (!client.Start()) {
    return 1;
  }
  if (options->mode == "cancel") {
    return client.RunCancel();
  }
  return client.RunSuccess();
}
