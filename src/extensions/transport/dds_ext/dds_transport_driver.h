// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "dds_ext/fastdds_compat.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/transport/serialized_transport_driver.h"
#include "dds_ext/dds_loan_bus.h"
#include "dds_ext/dds_bus.h"
#include "dds_ext/loaned_message.h"
#include "dds_ext/zero_copy_config_bus.h"
#include "dds_ext/zero_copy_monitor.h"
#include "cpp/executor/executor.h"

namespace ibmw::extensions::dds_extension {

class DdsQosTest;

/**
 * @brief Zero-copy statistics
 */
struct ZeroCopyStats {
  std::atomic<uint64_t> loan_success{0};
  std::atomic<uint64_t> loan_failure{0};
  std::atomic<uint64_t> zero_copy_publish{0};
  std::atomic<uint64_t> fallback_publish{0};

  ZeroCopyStats() = default;
  ZeroCopyStats(ZeroCopyStats&& other) noexcept
      : loan_success(other.loan_success.load()),
        loan_failure(other.loan_failure.load()),
        zero_copy_publish(other.zero_copy_publish.load()),
        fallback_publish(other.fallback_publish.load()) {}

  ZeroCopyStats(const ZeroCopyStats&) = delete;
  ZeroCopyStats& operator=(const ZeroCopyStats&) = delete;
  ZeroCopyStats& operator=(ZeroCopyStats&& other) noexcept {
    loan_success.store(other.loan_success.load());
    loan_failure.store(other.loan_failure.load());
    zero_copy_publish.store(other.zero_copy_publish.load());
    fallback_publish.store(other.fallback_publish.load());
    return *this;
  }

  double ZeroCopyRatio() const {
    uint64_t total = zero_copy_publish.load() + fallback_publish.load();
    if (total == 0) return 0.0;
    return static_cast<double>(zero_copy_publish.load()) / static_cast<double>(total);
  }
};

/**
 * @brief DDS transport driver
 *
 * Implements the TransportDriver interface using FastDDS for
 * DDS-native publish/subscribe with zero-copy support.
 */
class __attribute__((visibility("default"))) DdsTransportDriver
    : public runtime::core::transport::TransportDriver,
      public runtime::core::transport::SerializedTransportDriver {
 public:
  // QoS configuration
  struct QosOptions {
    std::string history = "keep_last";
    int32_t depth = 10;
    std::string reliability = "reliable";
    std::string durability = "volatile";
  };

  // Per-topic pub options
  struct PubTopicOptions {
    std::string topic_name;  // regex pattern
    QosOptions qos;
  };

  // Per-topic sub options
  struct SubTopicOptions {
    std::string topic_name;  // regex pattern
    QosOptions qos;
    std::string executor;
    std::string loan_mode = "auto";
  };

  // Extension options
  struct Options {
    QosOptions default_pub_qos;
    QosOptions default_sub_qos;
    std::vector<PubTopicOptions> pub_topics_options;
    std::vector<SubTopicOptions> sub_topics_options;
    bool ros2_compatible = false;
    std::string ros2_topic_prefix = "rt";
    std::string qos_preset;
    // Independent gate for FastDDS DataSharing (intra-host SHM zero-copy).
    // Decoupled from ros2_compatible so non-ROS2 deployments can opt in.
    // Requires a Plain CDR type to actually engage (FastDDS will silently
    // fall back to Shared/Intraprocess otherwise).
    bool enable_data_sharing = false;
  };

 public:
  explicit DdsTransportDriver(DdsBus& dds_bus);
  ~DdsTransportDriver() override = default;

  std::string_view Name() const noexcept override { return "dds"; }
  IBMW_DECLARE_DRIVER_SLOT("ibmw.transport.dds")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept override;

  bool RegisterSendType(
      const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool OnMessage(
      const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

  runtime::core::transport::SerializedSubscriberHandle
  AttachSerializedSubscriber(
      const runtime::core::transport::SerializedSubscriberOptions& options,
      runtime::core::transport::SerializedMessageCallback callback)
      noexcept;
  bool DetachSerializedSubscriber(
      runtime::core::transport::SerializedSubscriberHandle handle)
      noexcept;
  runtime::core::transport::SerializedPublisherHandle
  RegisterSerializedPublisher(
      const runtime::core::transport::SerializedPublisherOptions& options)
      noexcept;
  bool SendSerialized(
      runtime::core::transport::SerializedPublisherHandle handle,
      const runtime::core::transport::SerializedMessage& message)
      noexcept;
  bool WaitForSerializedAcknowledgments(
      std::chrono::milliseconds timeout) noexcept;

  runtime::core::transport::QosDescriptor PublisherQos(
      std::string_view topic,
      std::string_view msg_type) const noexcept;
  runtime::core::transport::QosDescriptor SubscriberQos(
      std::string_view topic,
      std::string_view msg_type) const noexcept;

  // ----- Phase 7 §7.3 §K-3.6: user-level zero-copy loan fast lane -----
  //
  // FastDDS native loan path. Supported only for kPodOnly mode and only on
  // PlainCdr-eligible types (publisher_map_[topic].is_plain == true). The
  // CDR wire format differs from the native struct layout; ctx-meta loans
  // are not implementable on FastDDS without a full re-serialize, so the
  // ctx-meta mode returns false here. The caller (LoanedSample<T> / typed
  // SenderAccess<T>::Loan) handles the failure by yielding an empty wrapper
  // and the dispatcher tries the next driver.
  //
  // Loan returns a chunk of size (publisher_map_[topic].type_size - 4) — the
  // CDR encapsulation header occupies the first 4 bytes of the FastDDS
  // PlainCdr serialized form, so the user-visible payload region begins at
  // chunk + 4. Requests larger than this are rejected (FastDDS loan_sample
  // takes no size parameter; chunk size is fixed by the type).
  bool LoanSample(
      const runtime::core::transport::TransportInfo& info,
      size_t requested_size,
      runtime::core::transport::LoanMode mode,
      const ibmw::transport::Context* ctx,
      runtime::core::transport::LoanedSampleHandle& out_handle) noexcept override;
  void PublishLoaned(
      runtime::core::transport::LoanedSampleHandle& handle) noexcept override;
  void ReleaseLoaned(
      runtime::core::transport::LoanedSampleHandle& handle) noexcept override;

  // Executor registration
  using GetExecutorFunc = std::function<ibmw::executor::ExecutorHandle(std::string_view)>;
  void RegisterGetExecutorFunc(GetExecutorFunc func) { get_executor_func_ = std::move(func); }

  // Zero-copy API helpers
  bool IsPlainType(const std::string& dds_topic_name) const;
  bool CanLoanMessages(const std::string& dds_topic_name) const;
  eprosima::fastdds::dds::DataWriter* GetDataWriter(const std::string& dds_topic_name);
  // Returns -1 if the topic is unknown or the DDS status query fails.
  int GetPublicationMatchedCount(const std::string& dds_topic_name) const;
  const ZeroCopyStats* GetZeroCopyStats(const std::string& dds_topic_name) const;
  void UpdateStats(const std::string& dds_topic_name, bool is_zero_copy);
  const ZeroCopyConfig& GetZeroCopyConfig(const std::string& dds_topic_name) const;
  const ZeroCopyConfigBus& GetConfigBus() const { return zero_copy_config_mgr_; }
  const Options& GetOptions() const { return options_; }

 private:
  friend class DdsQosTest;

  // DDS data reader listener
  class DdsDataReaderListener : public eprosima::fastdds::dds::DataReaderListener {
   public:
    DdsDataReaderListener(
        std::shared_ptr<runtime::core::transport::MessageHandlerTool> message_handler_tool,
        const runtime::core::transport::MessageHandlerWrapper& message_handler,
        bool is_plain,
        ibmw::executor::ExecutorHandle executor,
        LoanMode loan_mode)
        : message_handler_tool_(std::move(message_handler_tool)),
          message_handler_(message_handler),
          is_plain_(is_plain),
          executor_(executor),
          loan_mode_(loan_mode) {}

    void on_data_available(eprosima::fastdds::dds::DataReader* reader) override;
    void on_requested_deadline_missed(
        eprosima::fastdds::dds::DataReader* reader,
        const fastdds_compat::RequestedDeadlineMissedStatus& status) override;
    void on_sample_rejected(
        eprosima::fastdds::dds::DataReader* reader,
        const fastdds_compat::SampleRejectedStatus& status) override;
    void on_sample_lost(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::SampleLostStatus& status) override;
    bool WaitForAsyncCallbacks(std::chrono::milliseconds timeout) const;
    void StopReclaimerAndDrain();
    void LogAsyncProfileOnShutdown(std::string_view reason) const;
    static bool IsAsyncProfileEnabled() noexcept;

   private:
    struct AsyncCallbackState {
      std::atomic<int> pending{0};
      mutable std::mutex mutex;
      mutable std::condition_variable cv;
    };

    struct NonPlainProfileStats {
      std::atomic<uint64_t> on_data_available_count{0};
      std::atomic<uint64_t> on_data_available_last_ns{0};
      std::atomic<uint64_t> on_data_available_interarrival_count{0};
      std::atomic<uint64_t> on_data_available_interarrival_total_ns{0};
      std::atomic<uint64_t> on_data_available_interarrival_max_ns{0};
      std::atomic<uint64_t> create_count{0};
      std::atomic<uint64_t> create_total_ns{0};
      std::atomic<uint64_t> create_max_ns{0};
      std::atomic<uint64_t> take_count{0};
      std::atomic<uint64_t> take_total_ns{0};
      std::atomic<uint64_t> take_max_ns{0};
      std::atomic<uint64_t> valid_samples{0};
      std::atomic<uint64_t> dispatch_count{0};
      std::atomic<uint64_t> dispatch_failures{0};
      std::atomic<uint64_t> dispatch_enqueue_count{0};
      std::atomic<uint64_t> dispatch_enqueue_total_ns{0};
      std::atomic<uint64_t> dispatch_enqueue_max_ns{0};
      std::atomic<uint64_t> dispatch_wakeup_count{0};
      std::atomic<uint64_t> dispatch_wakeup_total_ns{0};
      std::atomic<uint64_t> dispatch_wakeup_max_ns{0};
      std::atomic<uint64_t> callback_before_dispatch_return{0};
      std::atomic<uint64_t> callback_total_ns{0};
      std::atomic<uint64_t> callback_max_ns{0};
      static constexpr size_t kLatencyBucketCount =
          DdsLoanedAsyncProfileStats::kLatencyBucketCount;
      std::array<std::atomic<uint64_t>, kLatencyBucketCount> take_buckets{};
      std::array<std::atomic<uint64_t>, kLatencyBucketCount> on_data_available_interarrival_buckets{};
      std::array<std::atomic<uint64_t>, kLatencyBucketCount> dispatch_enqueue_buckets{};
      std::array<std::atomic<uint64_t>, kLatencyBucketCount> dispatch_wakeup_buckets{};
      std::array<std::atomic<uint64_t>, kLatencyBucketCount> callback_buckets{};
    };

    void HandlePlainType(eprosima::fastdds::dds::DataReader* reader);
    void HandleNonPlainType(eprosima::fastdds::dds::DataReader* reader);
    static void FinishAsyncCallback(const std::shared_ptr<AsyncCallbackState>& state);
    static void IncrementAsyncPending(
        const std::shared_ptr<AsyncCallbackState>& state,
        const std::shared_ptr<DdsLoanedAsyncProfileStats>& stats);
    static void LogAsyncProfileSnapshot(
        const std::shared_ptr<DdsLoanedAsyncProfileStats>& stats,
        std::string_view reason,
        uint64_t pending_current);
    static bool IsReaderStatusProfileEnabled() noexcept;
    static bool IsNonPlainProfileEnabled() noexcept;
    static void LogNonPlainProfileSnapshot(
        const std::shared_ptr<NonPlainProfileStats>& stats,
        std::string_view reason,
        uint64_t pending_current);

    std::shared_ptr<runtime::core::transport::MessageHandlerTool> message_handler_tool_;
    const runtime::core::transport::MessageHandlerWrapper& message_handler_;
    bool is_plain_;
    ibmw::executor::ExecutorHandle executor_;
    LoanMode loan_mode_;
    std::shared_ptr<AsyncCallbackState> async_state_{std::make_shared<AsyncCallbackState>()};
    std::shared_ptr<std::mutex> reader_mutex_{std::make_shared<std::mutex>()};
    std::shared_ptr<DdsLoanReclaimer> loan_reclaimer_;
    std::shared_ptr<DdsLoanedAsyncProfileStats> async_profile_stats_{
        loan_mode_ == LoanMode::ASYNC && IsAsyncProfileEnabled()
            ? std::make_shared<DdsLoanedAsyncProfileStats>()
            : nullptr};
    std::shared_ptr<NonPlainProfileStats> nonplain_profile_stats_{
        !is_plain_ && IsNonPlainProfileEnabled()
            ? std::make_shared<NonPlainProfileStats>()
            : nullptr};
  };

  // Publisher info
  struct PublishProfileStats {
    std::atomic<uint64_t> nonplain_write_count{0};
    std::atomic<uint64_t> nonplain_write_total_ns{0};
    std::atomic<uint64_t> nonplain_write_max_ns{0};
    std::atomic<uint64_t> nonplain_write_failures{0};
  };

  struct PublisherInfo {
    eprosima::fastdds::dds::Topic* topic = nullptr;
    eprosima::fastdds::dds::DataWriter* writer = nullptr;
    eprosima::fastdds::dds::TypeSupport native_type_support;
    bool is_plain = false;
    uint32_t type_size = 0;
    ZeroCopyStats stats;
    std::shared_ptr<PublishProfileStats> publish_profile_stats;
  };

  // Subscriber info
  struct SubscriberInfo {
    eprosima::fastdds::dds::Topic* topic = nullptr;
    eprosima::fastdds::dds::DataReader* reader = nullptr;
    std::shared_ptr<runtime::core::transport::MessageHandlerTool> message_handler_tool;
    std::unique_ptr<DdsDataReaderListener> listener;
    ibmw::executor::ExecutorHandle executor;
    LoanMode loan_mode = LoanMode::AUTO;
  };

  struct SerializedSubscriberInfo {
    eprosima::fastdds::dds::Topic* topic = nullptr;
    eprosima::fastdds::dds::DataReader* reader = nullptr;
    eprosima::fastdds::dds::TypeSupport native_type_support;
    std::unique_ptr<eprosima::fastdds::dds::DataReaderListener> listener;
  };

  struct SerializedPublisherInfo {
    runtime::core::transport::TopicDescriptor descriptor;
    eprosima::fastdds::dds::Topic* topic = nullptr;
    eprosima::fastdds::dds::DataWriter* writer = nullptr;
    eprosima::fastdds::dds::TypeSupport native_type_support;
  };

 private:
  std::string GetDdsTopicName(
      const std::string& ibmw_topic,
      const std::string& msg_type) const;

  eprosima::fastdds::dds::DataWriterQos GetWriterQos(
      const std::string& topic_name,
      const std::string& dds_topic_name) const;
  eprosima::fastdds::dds::DataReaderQos GetReaderQos(
      const std::string& topic_name,
      const std::string& dds_topic_name) const;

  void SendPlainType(
      runtime::core::transport::MessageFrame& msg_wrapper,
      PublisherInfo& pub_info,
      const std::string& dds_topic_name);
  void SendNonPlainType(
      runtime::core::transport::MessageFrame& msg_wrapper,
      PublisherInfo& pub_info,
      const std::string& dds_topic_name);

  std::pair<ibmw::executor::ExecutorHandle, LoanMode> GetExecutorAndLoanMode(
      const std::string& topic_name) const;

  static bool IsRos2DdsType(const std::string& dds_type_name);
  static bool IsPublishProfileEnabled() noexcept;
  static void LogPublishProfileSnapshot(
      const std::shared_ptr<PublishProfileStats>& stats,
      std::string_view reason,
      std::string_view dds_topic_name);

 private:
  DdsBus& dds_bus_;
  Options options_;

  eprosima::fastdds::dds::Publisher* publisher_ = nullptr;
  eprosima::fastdds::dds::Subscriber* subscriber_ = nullptr;

  // topic_name -> PublisherInfo
  std::unordered_map<std::string, PublisherInfo> publisher_map_;
  // topic_name -> SubscriberInfo
  std::unordered_map<std::string, SubscriberInfo> subscriber_map_;
  std::mutex serialized_subscriber_mutex_;
  std::unordered_map<uint64_t, SerializedSubscriberInfo>
      serialized_subscriber_map_;
  std::atomic<uint64_t> next_serialized_subscriber_handle_{1};
  std::mutex serialized_publisher_mutex_;
  std::unordered_map<uint64_t, SerializedPublisherInfo>
      serialized_publisher_map_;
  std::atomic<uint64_t> next_serialized_publisher_handle_{1};
  // DDS topic_name -> ibmw topic_name (for reverse lookup)
  std::unordered_map<std::string, std::string> dds_topic_to_ibmw_topic_;
  // registered DDS topics
  std::unordered_map<std::string, eprosima::fastdds::dds::Topic*> topic_map_;

  GetExecutorFunc get_executor_func_;

  ZeroCopyConfigBus zero_copy_config_mgr_;
  std::unique_ptr<ZeroCopyMonitor> zero_copy_monitor_;

  enum class State { kPreInit, kInit, kStart, kShutdown };
  State state_ = State::kPreInit;
};

}  // namespace ibmw::extensions::dds_extension
