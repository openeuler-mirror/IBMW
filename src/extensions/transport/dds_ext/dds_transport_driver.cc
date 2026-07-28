// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/global.h"
#include "dds_ext/ros2_type_helper.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/publisher/qos/PublisherQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>

#if __has_include(<fastdds/rtps/common/InstanceHandle.hpp>)
#include <fastdds/rtps/common/InstanceHandle.hpp>
#else
#include <fastdds/rtps/common/InstanceHandle.h>
#endif

#include "base/time_util.h"

// Compatibility for ReturnCode_t (FastDDS 3.x vs FastRTPS 2.x)
#if __has_include(<fastdds/dds/core/detail/DDSReturnCode.hpp>)
  // FastDDS 3.x
  namespace compat_dds = eprosima::fastdds::dds;
  inline bool CompatWriteSuccess(const eprosima::fastdds::dds::ReturnCode_t& ret) {
    return ret == eprosima::fastdds::dds::RETCODE_OK;
  }
  inline bool CompatWriteSuccess(bool ret) {
    return ret;
  }
  inline std::string CompatWriteResultString(const eprosima::fastdds::dds::ReturnCode_t& ret) {
    std::ostringstream oss;
    oss << ret;
    return oss.str();
  }
  inline std::string CompatWriteResultString(bool ret) {
    return ret ? "true" : "false";
  }
  #define COMPAT_IS_PLAIN(type) (type)->is_plain(eprosima::fastdds::dds::DEFAULT_DATA_REPRESENTATION)
  #define COMPAT_TYPE_SIZE(type) (type)->max_serialized_type_size
  #define COMPAT_SERIALIZE(ts, data, payload) (ts)->serialize((data), (payload), eprosima::fastdds::dds::DEFAULT_DATA_REPRESENTATION)
  #define COMPAT_DESERIALIZE(ts, payload, data) (ts)->deserialize((payload), (data))
  using compat_SerializedPayload_t = eprosima::fastdds::rtps::SerializedPayload_t;
  #define COMPAT_WRITE_SUCCESS(ret) (CompatWriteSuccess(ret))
#else
  // FastRTPS 2.x (e.g., ROS2 Humble)
  namespace compat_dds {
    using ReturnCode_t = eprosima::fastrtps::types::ReturnCode_t;
    constexpr auto RETCODE_OK = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
    constexpr auto RETCODE_NO_DATA = eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA;
  }
  #define COMPAT_IS_PLAIN(type) (type)->is_plain()
  #define COMPAT_TYPE_SIZE(type) (type)->m_typeSize
  #define COMPAT_SERIALIZE(ts, data, payload) (ts)->serialize((data), &(payload))
  #define COMPAT_DESERIALIZE(ts, payload, data) (ts)->deserialize(&(payload), (data))
  using compat_SerializedPayload_t = eprosima::fastrtps::rtps::SerializedPayload_t;
  inline bool CompatWriteSuccess(bool ret) {
    return ret;
  }
  inline std::string CompatWriteResultString(bool ret) {
    return ret ? "true" : "false";
  }
  inline bool CompatWriteSuccess(const compat_dds::ReturnCode_t& ret) {
    return ret == compat_dds::RETCODE_OK;
  }
  inline std::string CompatWriteResultString(const compat_dds::ReturnCode_t& ret) {
    return std::to_string(ret());
  }
  #define COMPAT_WRITE_SUCCESS(ret) (CompatWriteSuccess(ret))
#endif

namespace YAML {
template <>
struct convert<ibmw::extensions::dds_extension::DdsTransportDriver::QosOptions> {
  using QosOptions = ibmw::extensions::dds_extension::DdsTransportDriver::QosOptions;

  static Node encode(const QosOptions& rhs) {
    Node node;
    node["history"] = rhs.history;
    node["depth"] = rhs.depth;
    node["reliability"] = rhs.reliability;
    node["durability"] = rhs.durability;
    return node;
  }

  static bool decode(const Node& node, QosOptions& rhs) {
    if (node["history"])
      rhs.history = node["history"].as<std::string>();
    if (node["depth"])
      rhs.depth = node["depth"].as<int32_t>();
    if (node["reliability"])
      rhs.reliability = node["reliability"].as<std::string>();
    if (node["durability"])
      rhs.durability = node["durability"].as<std::string>();
    return true;
  }
};

template <>
struct convert<ibmw::extensions::dds_extension::DdsTransportDriver::Options> {
  using Options = ibmw::extensions::dds_extension::DdsTransportDriver::Options;
  using QosOptions = ibmw::extensions::dds_extension::DdsTransportDriver::QosOptions;
  using PubTopicOptions = ibmw::extensions::dds_extension::DdsTransportDriver::PubTopicOptions;
  using SubTopicOptions = ibmw::extensions::dds_extension::DdsTransportDriver::SubTopicOptions;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_opt : rhs.pub_topics_options) {
      Node opt_node;
      opt_node["topic"] = pub_opt.topic_name;
      opt_node["qos"] = pub_opt.qos;
      node["publishers"].push_back(opt_node);
    }

    node["subscribers"] = YAML::Node();
    for (const auto& sub_opt : rhs.sub_topics_options) {
      Node opt_node;
      opt_node["topic"] = sub_opt.topic_name;
      opt_node["qos"] = sub_opt.qos;
      node["subscribers"].push_back(opt_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& opt_node : node["publishers"]) {
        PubTopicOptions pub_opt;
        pub_opt.topic_name = opt_node["topic"].as<std::string>();
        if (opt_node["qos"])
          pub_opt.qos = opt_node["qos"].as<QosOptions>();
        rhs.pub_topics_options.emplace_back(std::move(pub_opt));
      }
    }

    if (node["subscribers"] && node["subscribers"].IsSequence()) {
      for (const auto& opt_node : node["subscribers"]) {
        SubTopicOptions sub_opt;
        sub_opt.topic_name = opt_node["topic"].as<std::string>();
        if (opt_node["qos"])
          sub_opt.qos = opt_node["qos"].as<QosOptions>();
        if (opt_node["executor"])
          sub_opt.executor = opt_node["executor"].as<std::string>();
        if (opt_node["loan_mode"])
          sub_opt.loan_mode = opt_node["loan_mode"].as<std::string>();
        rhs.sub_topics_options.emplace_back(std::move(sub_opt));
      }
    }

    if (node["ros2_compatible"])
      rhs.ros2_compatible = node["ros2_compatible"].as<bool>();
    if (node["ros2_topic_prefix"])
      rhs.ros2_topic_prefix = node["ros2_topic_prefix"].as<std::string>();
    if (node["qos_preset"])
      rhs.qos_preset = node["qos_preset"].as<std::string>();
    if (node["enable_data_sharing"])
      rhs.enable_data_sharing = node["enable_data_sharing"].as<bool>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::dds_extension {

namespace {

struct RawCdrSample {
  std::vector<uint8_t> bytes;
};

class RawCdrTopicDataType final
    : public eprosima::fastdds::dds::TopicDataType {
 public:
  explicit RawCdrTopicDataType(const std::string& type_name) {
#if IBMW_FASTDDS_V3_PLUS
    set_name(type_name);
    max_serialized_type_size = 64u * 1024u * 1024u;
    is_compute_key_provided = false;
#else
    setName(type_name.c_str());
    m_typeSize = 64u * 1024u * 1024u;
    m_isGetKeyDefined = false;
    auto_fill_type_object(false);
    auto_fill_type_information(false);
#endif
  }

#if IBMW_FASTDDS_V3_PLUS
  bool serialize(
      const void* const data,
      eprosima::fastdds::rtps::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t) override {
    return CopyToPayload(*static_cast<const RawCdrSample*>(data), payload);
  }

  bool deserialize(
      eprosima::fastdds::rtps::SerializedPayload_t& payload,
      void* data) override {
    return CopyFromPayload(payload, *static_cast<RawCdrSample*>(data));
  }

  uint32_t calculate_serialized_size(
      const void* const data,
      eprosima::fastdds::dds::DataRepresentationId_t) override {
    return static_cast<uint32_t>(
        static_cast<const RawCdrSample*>(data)->bytes.size());
  }

  void* create_data() override { return new RawCdrSample(); }
  void delete_data(void* data) override {
    delete static_cast<RawCdrSample*>(data);
  }
  bool compute_key(
      eprosima::fastdds::rtps::SerializedPayload_t&,
      eprosima::fastdds::rtps::InstanceHandle_t&,
      bool) override {
    return false;
  }
  bool compute_key(
      const void* const,
      eprosima::fastdds::rtps::InstanceHandle_t&,
      bool) override {
    return false;
  }
#else
  bool serialize(
      void* data,
      eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
    return CopyToPayload(
        *static_cast<const RawCdrSample*>(data), *payload);
  }

  bool deserialize(
      eprosima::fastrtps::rtps::SerializedPayload_t* payload,
      void* data) override {
    return CopyFromPayload(*payload, *static_cast<RawCdrSample*>(data));
  }

  std::function<uint32_t()> getSerializedSizeProvider(void* data) override {
    return [sample = static_cast<RawCdrSample*>(data)] {
      return static_cast<uint32_t>(sample->bytes.size());
    };
  }

  void* createData() override { return new RawCdrSample(); }
  void deleteData(void* data) override {
    delete static_cast<RawCdrSample*>(data);
  }
  bool getKey(
      void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
    return false;
  }
#endif

 private:
  template <typename Payload>
  static bool CopyToPayload(const RawCdrSample& sample, Payload& payload) {
    if (sample.bytes.size() > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    const auto size = static_cast<uint32_t>(sample.bytes.size());
    if (payload.max_size < size) {
      payload.reserve(size);
      if (payload.max_size < size) {
        return false;
      }
    }
    if (size != 0) {
      std::memcpy(payload.data, sample.bytes.data(), size);
    }
    payload.length = size;
    return true;
  }

  template <typename Payload>
  static bool CopyFromPayload(const Payload& payload, RawCdrSample& sample) {
    sample.bytes.assign(payload.data, payload.data + payload.length);
    return true;
  }
};

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    std::vector<uint8_t> bytes) {
  auto storage = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view, [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

class SerializedDataReaderListener final
    : public eprosima::fastdds::dds::DataReaderListener {
 public:
  SerializedDataReaderListener(
      runtime::core::transport::TopicDescriptor descriptor,
      runtime::core::transport::SerializedMessageCallback callback)
      : descriptor_(std::move(descriptor)), callback_(std::move(callback)) {}

  void on_data_available(
      eprosima::fastdds::dds::DataReader* reader) override {
    RawCdrSample sample;
    eprosima::fastdds::dds::SampleInfo info;
    while (reader->take_next_sample(&sample, &info) == compat_dds::RETCODE_OK) {
      if (!info.valid_data) {
        continue;
      }
      std::ostringstream publisher;
      publisher << info.sample_identity.writer_guid();
      const uint64_t sequence =
          info.sample_identity.sequence_number().to64long();
      const auto publish_ns = info.source_timestamp.to_ns();
      try {
        callback_(runtime::core::transport::SerializedMessage{
            .topic = descriptor_,
            .identity =
                {.source_id = "dds",
                 .publisher_id = publisher.str(),
                 .sequence = sequence},
            .publish_timestamp =
                publish_ns > 0 ? static_cast<uint64_t>(publish_ns) : 0,
            .receive_timestamp = ibmw::common::util::GetCurTimestampNs(),
            .source_driver = "dds",
            .payload = MakeOwnedPayload(std::move(sample.bytes))});
      } catch (const std::exception& error) {
        MW_ERROR("Raw DDS subscriber callback failed: {}", error.what());
      } catch (...) {
        MW_ERROR("Raw DDS subscriber callback failed.");
      }
      sample.bytes.clear();
    }
  }

 private:
  runtime::core::transport::TopicDescriptor descriptor_;
  runtime::core::transport::SerializedMessageCallback callback_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

DdsTransportDriver::DdsTransportDriver(DdsBus& dds_bus)
    : dds_bus_(dds_bus) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DdsTransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      state_ == State::kPreInit,
      "DDS transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  for (const auto& sub_opt : options_.sub_topics_options) {
    LoanMode configured_loan_mode = ParseLoanMode(sub_opt.loan_mode);
    if (!sub_opt.executor.empty()) {
      MW_REQUIRE(
          get_executor_func_,
          "DDS subscriber executor '{}' configured for topic pattern '{}' but executor lookup function is not set.",
          sub_opt.executor,
          sub_opt.topic_name);

      auto executor = get_executor_func_(sub_opt.executor);
      MW_REQUIRE(
          executor,
          "Invalid DDS subscriber executor '{}' for topic pattern '{}'.",
          sub_opt.executor,
          sub_opt.topic_name);

      if (configured_loan_mode == LoanMode::SYNC) {
        MW_WARN(
            "DDS subscriber executor '{}' for topic pattern '{}' is ignored because loan_mode is 'sync'.",
            sub_opt.executor,
            sub_opt.topic_name);
      }
    } else {
      MW_REQUIRE(
          configured_loan_mode != LoanMode::ASYNC,
          "DDS subscriber loan_mode 'async' for topic pattern '{}' requires subscribers[].executor.",
          sub_opt.topic_name);
    }
  }

  // Load zero-copy configuration
  if (options_node && !options_node.IsNull()) {
    zero_copy_config_mgr_.LoadGlobalConfig(options_node);

    if (options_node["publishers"]) {
      for (const auto& topic_opt : options_node["publishers"]) {
        if (topic_opt["topic"]) {
          std::string topic_name = topic_opt["topic"].as<std::string>();
          zero_copy_config_mgr_.LoadTopicConfig(topic_name, topic_opt);
        }
      }
    }
    if (options_node["subscribers"]) {
      for (const auto& topic_opt : options_node["subscribers"]) {
        if (topic_opt["topic"]) {
          std::string topic_name = topic_opt["topic"].as<std::string>();
          zero_copy_config_mgr_.LoadTopicConfig(topic_name, topic_opt);
        }
      }
    }
  }

  // Create publisher and subscriber
  publisher_ = dds_bus_.GetParticipant()->create_publisher(
      eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT);

  subscriber_ = dds_bus_.GetParticipant()->create_subscriber(
      eprosima::fastdds::dds::SUBSCRIBER_QOS_DEFAULT);

  state_ = State::kInit;

  MW_INFO("DDS transport driver initialized.");
}

void DdsTransportDriver::Start() {
  MW_REQUIRE(
      state_ == State::kInit,
      "Method can only be called when state is 'Init'.");

  // Start zero-copy monitoring if enabled
  const auto& global_config = zero_copy_config_mgr_.GetGlobalConfig();
  if (global_config.monitoring_enabled) {
    zero_copy_monitor_ = std::make_unique<ZeroCopyMonitor>([this]() {
      MW_INFO("========== Zero-Copy Statistics ==========");
      for (const auto& [topic, pub_info] : publisher_map_) {
        auto ratio = pub_info.stats.ZeroCopyRatio();
        MW_INFO("  [PUB] {} : loan_ok={}, loan_fail={}, zc={}, fallback={}, ratio={:.1f}%",
                   topic,
                   pub_info.stats.loan_success.load(),
                   pub_info.stats.loan_failure.load(),
                   pub_info.stats.zero_copy_publish.load(),
                   pub_info.stats.fallback_publish.load(),
                   ratio * 100.0);
      }
      MW_INFO("==========================================");
    });
    zero_copy_monitor_->Start(global_config.monitoring_interval_sec);
  }

  state_ = State::kStart;
  MW_INFO("DDS transport driver started.");
}

void DdsTransportDriver::Shutdown() {
  if (state_ == State::kShutdown)
    return;

  state_ = State::kShutdown;

  const bool shutdown_profile = DdsDataReaderListener::IsAsyncProfileEnabled();
  if (shutdown_profile) {
    MW_INFO(
        "DDS shutdown profile [enter]: data_sharing={} subscribers={} publishers={} topics={}",
        options_.enable_data_sharing,
        subscriber_map_.size(),
        publisher_map_.size(),
        topic_map_.size());
  }

  // Stop zero-copy monitoring
  if (zero_copy_monitor_) {
    if (shutdown_profile) {
      MW_INFO("DDS shutdown profile [zero-copy-monitor-stop-start]");
    }
    zero_copy_monitor_->Stop();
    zero_copy_monitor_.reset();
    if (shutdown_profile) {
      MW_INFO("DDS shutdown profile [zero-copy-monitor-stop-done]");
    }
  }

  {
    std::lock_guard lock(serialized_subscriber_mutex_);
    for (auto& [handle, sub_info] : serialized_subscriber_map_) {
      static_cast<void>(handle);
      if (sub_info.reader) {
        sub_info.reader->set_listener(nullptr);
        subscriber_->delete_datareader(sub_info.reader);
      }
    }
    serialized_subscriber_map_.clear();
  }

  // Detach listeners before draining. FastDDS DataSharing can otherwise start
  // a fresh on_data_available while shutdown is waiting on the previous batch,
  // and delete_datareader may deadlock with a callback thread returning a loan.
  const bool abandon_fastdds_teardown =
      options_.enable_data_sharing && !subscriber_map_.empty();

  if (shutdown_profile) {
    MW_INFO(
        "DDS shutdown profile [listener-detach-start]: subscribers={} abandon_fastdds_teardown={}",
        subscriber_map_.size(),
        abandon_fastdds_teardown);
  }
  for (auto& [key, sub_info] : subscriber_map_) {
    if (sub_info.reader) {
      sub_info.reader->set_listener(nullptr);
    }
  }
  if (shutdown_profile) {
    MW_INFO("DDS shutdown profile [listener-detach-done]");
  }

  // Clean up subscribers after the callback/loan drain has reached a stable
  // zero. The listener has already been detached, so no new loan batches can be
  // created while we wait.
  for (auto& [key, sub_info] : subscriber_map_) {
    if (sub_info.reader) {
      bool drained = true;
      if (sub_info.listener) {
        sub_info.listener->LogAsyncProfileOnShutdown("shutdown-start");
        drained = sub_info.listener->WaitForAsyncCallbacks(std::chrono::milliseconds(5000));
        if (drained) {
          sub_info.listener->StopReclaimerAndDrain();
        } else {
          sub_info.listener->LogAsyncProfileOnShutdown("shutdown-timeout");
        }
      }

      if (!drained) {
        MW_ERROR(
            "DDS subscriber '{}' still has async callbacks in flight; skipping DataReader deletion to avoid loan UAF.",
            key);
        (void)sub_info.listener.release();
        continue;
      }

      if (abandon_fastdds_teardown) {
        continue;
      }

      auto ret = subscriber_->delete_datareader(sub_info.reader);
      if (ret != compat_dds::RETCODE_OK) {
        MW_ERROR("Failed to delete DDS DataReader for '{}'.", key);
        (void)sub_info.listener.release();
      }
    }
  }

  if (abandon_fastdds_teardown) {
    subscriber_map_.clear();
    dds_bus_.AbandonShutdown();
    MW_WARN(
        "DDS DataSharing subscriber shutdown: skipped FastDDS entity deletion to avoid known DataSharing teardown deadlock; OS will reclaim resources.");
    MW_INFO("DDS transport driver shutdown.");
    return;
  }
  subscriber_map_.clear();

  // Clean up publishers after readers have been detached/deleted. This avoids
  // reliable/DataSharing writer teardown waiting on an active local reader.
  for (auto& [key, pub_info] : publisher_map_) {
    LogPublishProfileSnapshot(pub_info.publish_profile_stats, "shutdown", key);
    if (pub_info.writer) {
      publisher_->delete_datawriter(pub_info.writer);
    }
  }
  publisher_map_.clear();

  // Delete topics
  for (auto& [key, topic] : topic_map_) {
    if (topic) {
      dds_bus_.GetParticipant()->delete_topic(topic);
    }
  }
  topic_map_.clear();

  if (publisher_) {
    dds_bus_.GetParticipant()->delete_publisher(publisher_);
    publisher_ = nullptr;
  }

  if (subscriber_) {
    dds_bus_.GetParticipant()->delete_subscriber(subscriber_);
    subscriber_ = nullptr;
  }

  MW_INFO("DDS transport driver shutdown.");
}

// ---------------------------------------------------------------------------
// Initialization report
// ---------------------------------------------------------------------------

std::list<std::pair<std::string, std::string>>
DdsTransportDriver::GenInitializationReport() const noexcept {
  std::list<std::pair<std::string, std::string>> report;

  report.emplace_back("DDS publishers", std::to_string(publisher_map_.size()));
  report.emplace_back("DDS subscribers", std::to_string(subscriber_map_.size()));
  report.emplace_back("ROS2 compatible", options_.ros2_compatible ? "true" : "false");
  report.emplace_back("DataSharing enabled", options_.enable_data_sharing ? "true" : "false");

  return report;
}

// ---------------------------------------------------------------------------
// Topic naming
// ---------------------------------------------------------------------------

std::string DdsTransportDriver::GetDdsTopicName(
    const std::string& ibmw_topic, const std::string& msg_type) const {

  bool is_ros2_type = ROS2TypeHelper::IsROS2Type(msg_type);

  // ROS2 compatible mode
  if (options_.ros2_compatible && is_ros2_type) {
    return ROS2TypeHelper::ConvertToDdsTopicName(ibmw_topic, options_.ros2_topic_prefix);
  }

  // Native ibmw mode: encode type name into topic name
  std::string encoded_type = msg_type;
  std::replace(encoded_type.begin(), encoded_type.end(), ':', '_');
  std::replace(encoded_type.begin(), encoded_type.end(), '.', '_');

  return ibmw_topic + "_native_" + encoded_type;
}

// ---------------------------------------------------------------------------
// QoS helpers
// ---------------------------------------------------------------------------

static void ApplyQosOptions(
    const DdsTransportDriver::QosOptions& qos_opt,
    eprosima::fastdds::dds::DataWriterQos& qos) {

  if (qos_opt.history == "keep_last") {
    qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = qos_opt.depth;
    if (qos_opt.depth > 0) {
      // FastDDS requires resource limits to be large enough for the requested
      // KEEP_LAST depth. Preserve unlimited (-1) limits, but raise finite
      // defaults that would otherwise cap or reject the configured depth.
      if (qos.resource_limits().max_samples_per_instance >= 0 &&
          qos.resource_limits().max_samples_per_instance < qos_opt.depth) {
        qos.resource_limits().max_samples_per_instance = qos_opt.depth;
      }
      if (qos.resource_limits().max_samples >= 0 &&
          qos.resource_limits().max_samples < qos_opt.depth) {
        qos.resource_limits().max_samples = qos_opt.depth;
      }
    }
  } else if (qos_opt.history == "keep_all") {
    qos.history().kind = eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS;
  }

  if (qos_opt.reliability == "reliable") {
    qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
  } else if (qos_opt.reliability == "best_effort") {
    qos.reliability().kind = eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
  }

  if (qos_opt.durability == "volatile") {
    qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
  } else if (qos_opt.durability == "transient_local") {
    qos.durability().kind = eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;
  }
}

static void ApplyWriterResourceHeadroom(
    eprosima::fastdds::dds::DataWriterQos& qos) {
  if (qos.history().kind != eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS ||
      qos.reliability().kind != eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS) {
    return;
  }

  constexpr int32_t kSafeDefaultExtraSamples = 100;
  int32_t extra_samples = kSafeDefaultExtraSamples;
  const char* configured = std::getenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES");
  if (configured != nullptr && configured[0] != '\0') {
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(configured, &end, 10);
    if (errno == ERANGE || end == configured || *end != '\0' || parsed < -1 ||
        parsed > std::numeric_limits<int32_t>::max()) {
      MW_WARN(
          "Invalid IBMW_DDS_RELIABLE_EXTRA_SAMPLES='{}'; using safe default {}.",
          configured,
          kSafeDefaultExtraSamples);
    } else if (parsed == -1) {
      return;
    } else {
      extra_samples = static_cast<int32_t>(parsed);
    }
  }

  // Additional slots keep payload allocation available while a full writer
  // history or outstanding loans temporarily retain cache entries.
  qos.resource_limits().extra_samples = extra_samples;
}

static bool IsEnvFlagEnabled(const char* name) noexcept {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
}

static bool IsReliableLowLatencyProfileEnabled() noexcept {
  return IsEnvFlagEnabled("IBMW_DDS_RELIABLE_LOW_LATENCY_PROFILE");
}

static bool IsReliableTimingProfileEnabled() noexcept {
  return IsEnvFlagEnabled("IBMW_DDS_RELIABLE_TIMING_PROFILE") ||
         IsReliableLowLatencyProfileEnabled();
}

static bool IsSyncPublishProfileEnabled() noexcept {
  return IsEnvFlagEnabled("IBMW_DDS_SYNC_PUBLISH_PROFILE") ||
         IsReliableLowLatencyProfileEnabled();
}

static void ApplyReliableTimingProfile(
    eprosima::fastdds::dds::DataWriterQos& qos) {
  if (qos.reliability().kind != eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS) {
    return;
  }

  qos.reliable_writer_qos().times.IBMW_FASTDDS_WRITER_INITIAL_HEARTBEAT_DELAY = {0, 1000000};
  qos.reliable_writer_qos().times.IBMW_FASTDDS_WRITER_HEARTBEAT_PERIOD = {0, 20000000};
  qos.reliable_writer_qos().times.IBMW_FASTDDS_WRITER_NACK_RESPONSE_DELAY = {0, 1000000};
  qos.reliable_writer_qos().disable_heartbeat_piggyback = false;
}

static void ApplySyncPublishProfile(
    eprosima::fastdds::dds::DataWriterQos& qos) {
  if (qos.reliability().kind != eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS) {
    return;
  }

  qos.publish_mode().kind = eprosima::fastdds::dds::SYNCHRONOUS_PUBLISH_MODE;
}

static void ApplyReliableTimingProfile(
    eprosima::fastdds::dds::DataReaderQos& qos) {
  if (qos.reliability().kind != eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS) {
    return;
  }

  qos.reliable_reader_qos().times.IBMW_FASTDDS_READER_INITIAL_ACKNACK_DELAY = {0, 1000000};
  qos.reliable_reader_qos().times.IBMW_FASTDDS_READER_HEARTBEAT_RESPONSE_DELAY = {0, 1000000};
}

static void ApplyQosOptions(
    const DdsTransportDriver::QosOptions& qos_opt,
    eprosima::fastdds::dds::DataReaderQos& qos) {

  if (qos_opt.history == "keep_last") {
    qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    qos.history().depth = qos_opt.depth;
    if (qos_opt.depth > 0) {
      // FastDDS requires resource limits to be large enough for the requested
      // KEEP_LAST depth. Preserve unlimited (-1) limits, but raise finite
      // defaults that would otherwise cap or reject the configured depth.
      if (qos.resource_limits().max_samples_per_instance >= 0 &&
          qos.resource_limits().max_samples_per_instance < qos_opt.depth) {
        qos.resource_limits().max_samples_per_instance = qos_opt.depth;
      }
      if (qos.resource_limits().max_samples >= 0 &&
          qos.resource_limits().max_samples < qos_opt.depth) {
        qos.resource_limits().max_samples = qos_opt.depth;
      }
    }
  } else if (qos_opt.history == "keep_all") {
    qos.history().kind = eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS;
  }

  if (qos_opt.reliability == "reliable") {
    qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
  } else if (qos_opt.reliability == "best_effort") {
    qos.reliability().kind = eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
  }

  if (qos_opt.durability == "volatile") {
    qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
  } else if (qos_opt.durability == "transient_local") {
    qos.durability().kind = eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;
  }
}

eprosima::fastdds::dds::DataWriterQos DdsTransportDriver::GetWriterQos(
    const std::string& topic_name,
    const std::string& dds_topic_name) const {
  eprosima::fastdds::dds::DataWriterQos qos;

  // Start with defaults
  ApplyQosOptions(options_.default_pub_qos, qos);

  // Override with per-topic options
  for (const auto& pub_opt : options_.pub_topics_options) {
    try {
      const std::regex topic_pattern(pub_opt.topic_name);
      if (std::regex_match(topic_name, topic_pattern) ||
          std::regex_match(dds_topic_name, topic_pattern)) {
        ApplyQosOptions(pub_opt.qos, qos);
      }
    } catch (const std::exception& e) {
      MW_WARN("QoS regex error for '{}': {}", pub_opt.topic_name, e.what());
    }
  }

  if (IsReliableTimingProfileEnabled()) {
    ApplyReliableTimingProfile(qos);
  }
  if (IsSyncPublishProfileEnabled()) {
    ApplySyncPublishProfile(qos);
  }
  ApplyWriterResourceHeadroom(qos);

  return qos;
}

eprosima::fastdds::dds::DataReaderQos DdsTransportDriver::GetReaderQos(
    const std::string& topic_name,
    const std::string& dds_topic_name) const {
  eprosima::fastdds::dds::DataReaderQos qos;

  // Start with defaults
  ApplyQosOptions(options_.default_sub_qos, qos);

  // Override with per-topic options
  for (const auto& sub_opt : options_.sub_topics_options) {
    try {
      const std::regex topic_pattern(sub_opt.topic_name);
      if (std::regex_match(topic_name, topic_pattern) ||
          std::regex_match(dds_topic_name, topic_pattern)) {
        ApplyQosOptions(sub_opt.qos, qos);
      }
    } catch (const std::exception& e) {
      MW_WARN("QoS regex error for '{}': {}", sub_opt.topic_name, e.what());
    }
  }

  if (IsReliableTimingProfileEnabled()) {
    ApplyReliableTimingProfile(qos);
  }

  return qos;
}

// ---------------------------------------------------------------------------
// Helper: executor and loan mode
// ---------------------------------------------------------------------------

std::pair<ibmw::executor::ExecutorHandle, LoanMode>
DdsTransportDriver::GetExecutorAndLoanMode(const std::string& topic_name) const {
  ibmw::executor::ExecutorHandle executor;
  LoanMode loan_mode = LoanMode::SYNC;

  for (const auto& sub_opt : options_.sub_topics_options) {
    bool matched = false;
    try {
      matched = std::regex_match(topic_name, std::regex(sub_opt.topic_name));
    } catch (const std::regex_error& e) {
      MW_WARN("Regex error: {}", e.what());
      continue;
    }

    if (!matched) continue;

    LoanMode configured_loan_mode = ParseLoanMode(sub_opt.loan_mode);

    if (!sub_opt.executor.empty() && get_executor_func_) {
      executor = get_executor_func_(sub_opt.executor);
    }

    MW_REQUIRE(
        sub_opt.executor.empty() || executor,
        "Invalid DDS subscriber executor '{}' for topic '{}'.",
        sub_opt.executor,
        topic_name);

    loan_mode = ResolveEffectiveLoanMode(configured_loan_mode, static_cast<bool>(executor));

    MW_REQUIRE(
        loan_mode != LoanMode::ASYNC || executor,
        "DDS subscriber loan_mode 'async' for topic '{}' requires subscribers[].executor.",
        topic_name);

    if (executor && configured_loan_mode == LoanMode::SYNC) {
      MW_WARN(
          "DDS subscriber executor '{}' for topic '{}' is ignored because loan_mode is 'sync'.",
          sub_opt.executor,
          topic_name);
    } else if (executor) {
      MW_INFO("Using executor '{}' for topic '{}'.", sub_opt.executor, topic_name);
    }
    break;
  }

  return {executor, loan_mode};
}

// ---------------------------------------------------------------------------
// Helper: ROS2 DDS type detection
// ---------------------------------------------------------------------------

bool DdsTransportDriver::IsRos2DdsType(const std::string& dds_type_name) {
  return (dds_type_name.find("::dds_::") != std::string::npos) &&
         (!dds_type_name.empty() && dds_type_name.back() == '_');
}

// ---------------------------------------------------------------------------
// Helper: get or create DDS topic
// ---------------------------------------------------------------------------

static eprosima::fastdds::dds::Topic* GetOrCreateTopic(
    eprosima::fastdds::dds::DomainParticipant* participant,
    const std::string& dds_topic_name,
    const std::string& type_name,
    std::unordered_map<std::string, eprosima::fastdds::dds::Topic*>& topic_map) {

  auto it = topic_map.find(dds_topic_name);
  if (it != topic_map.end()) {
    return it->second;
  }

  // Check if already exists in participant
  auto* topic_desc = participant->lookup_topicdescription(dds_topic_name);
  if (topic_desc != nullptr) {
    auto* topic = dynamic_cast<eprosima::fastdds::dds::Topic*>(topic_desc);
    if (topic) {
      topic_map[dds_topic_name] = topic;
      return topic;
    }
    return nullptr;
  }

  // Create new topic
  auto* topic = participant->create_topic(
      dds_topic_name, type_name, eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);

  if (topic) {
    topic_map[dds_topic_name] = topic;
  }
  return topic;
}

// ---------------------------------------------------------------------------
// RegisterSendType
// ---------------------------------------------------------------------------

bool DdsTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_ == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = send_type_wrapper.info;

    bool is_ros2_type = ROS2TypeHelper::IsROS2Type(info.msg_type);

    eprosima::fastdds::dds::TypeSupport type;

    // Detect native DDS type: type name starts with "dds:" or is ROS2
    bool is_native_dds = (info.msg_type.substr(0, 4) == "dds:") || is_ros2_type;
    const void* custom_ptr = info.msg_type_support_ref.CustomTypeSupportPtr();

    if (is_native_dds && custom_ptr != nullptr) {
      type = *static_cast<const eprosima::fastdds::dds::TypeSupport*>(custom_ptr);

      if (is_ros2_type) {
        MW_INFO("Using ROS2 native type for topic '{}', type: '{}'.",
                   info.topic_name, info.msg_type);
      } else {
        MW_INFO("Using native DDS type for topic '{}', type: '{}'.",
                   info.topic_name, info.msg_type);
      }
    } else {
      MW_ERROR("Only native DDS mode is supported. Topic '{}', type: '{}'. "
                  "Use ibmw::transport::RegisterSendType<DdsType, DdsPubSubType>().",
                  info.topic_name, info.msg_type);
      return false;
    }

    std::string dds_topic_name = GetDdsTopicName(info.topic_name, info.msg_type);

    // Check if already registered
    if (publisher_map_.find(dds_topic_name) != publisher_map_.end()) {
      return true;
    }

    // Register type with participant
    type.register_type(dds_bus_.GetParticipant());

    if (is_ros2_type && options_.ros2_compatible) {
      MW_INFO("ROS2 compatible mode: ibmw topic '{}' -> DDS topic '{}', DDS type: '{}'.",
                 info.topic_name, dds_topic_name, type.get_type_name());
    }

    // Get or create topic
    auto* topic = GetOrCreateTopic(
        dds_bus_.GetParticipant(), dds_topic_name, type.get_type_name(), topic_map_);

    if (topic == nullptr) {
      MW_ERROR("Failed to create DDS topic: {}.", dds_topic_name);
      return false;
    }

    // Get QoS
    auto writer_qos = GetWriterQos(info.topic_name, dds_topic_name);

    // Writer-side DataSharing gate. Keep this explicit-only for ROS2-compatible
    // interop: stock ROS2 CLI/listener peers should negotiate the default
    // FastDDS data path unless the user deliberately opts into shared-memory
    // data sharing for a controlled deployment.
    if (options_.enable_data_sharing) {
      writer_qos.data_sharing().automatic();
      MW_INFO("DataSharing enabled for writer (ros2_compatible={}, enable_data_sharing={}).",
                 options_.ros2_compatible, options_.enable_data_sharing);
    }

    // Create DataWriter
    auto* writer = publisher_->create_datawriter(topic, writer_qos);

    if (writer == nullptr) {
      MW_ERROR("Failed to create DataWriter for topic: {}.", dds_topic_name);
      return false;
    }

    // Detect plain type
    bool is_plain = COMPAT_IS_PLAIN(type);
    uint32_t type_size = static_cast<uint32_t>(COMPAT_TYPE_SIZE(type));

    MW_INFO("Publisher registered: topic='{}', dds_topic='{}', is_plain={}, type_size={} bytes.",
               info.topic_name, dds_topic_name, is_plain, type_size);

    // Store publisher info
    PublisherInfo pub_info;
    pub_info.topic = topic;
    pub_info.writer = writer;
    pub_info.native_type_support = type;
    pub_info.is_plain = is_plain;
    pub_info.type_size = type_size;
    if (!is_plain && IsPublishProfileEnabled()) {
      pub_info.publish_profile_stats = std::make_shared<PublishProfileStats>();
    }

    publisher_map_.emplace(dds_topic_name, std::move(pub_info));
    dds_topic_to_ibmw_topic_[dds_topic_name] = info.topic_name;

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("RegisterSendType failed: {}.", e.what());
    return false;
  }
}

// ---------------------------------------------------------------------------
// Subscribe
// ---------------------------------------------------------------------------

bool DdsTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_ == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = message_handler.info;

    bool is_ros2_type = ROS2TypeHelper::IsROS2Type(info.msg_type);

    eprosima::fastdds::dds::TypeSupport type;

    bool is_native_dds = (info.msg_type.substr(0, 4) == "dds:") || is_ros2_type;
    const void* custom_ptr = info.msg_type_support_ref.CustomTypeSupportPtr();

    if (is_native_dds && custom_ptr != nullptr) {
      type = *static_cast<const eprosima::fastdds::dds::TypeSupport*>(custom_ptr);

      if (is_ros2_type) {
        MW_INFO("Using ROS2 native type for subscription '{}', type: '{}'.",
                   info.topic_name, info.msg_type);
      } else {
        MW_INFO("Using native DDS type for subscription '{}', type: '{}'.",
                   info.topic_name, info.msg_type);
      }
    } else {
      MW_ERROR("Only native DDS mode is supported. Subscription '{}', type: '{}'. "
                  "Use ibmw::transport::Subscribe<DdsType, DdsPubSubType>().",
                  info.topic_name, info.msg_type);
      return false;
    }

    std::string dds_topic_name = GetDdsTopicName(info.topic_name, info.msg_type);

    // Check if already subscribed - add to existing tool
    auto sub_it = subscriber_map_.find(dds_topic_name);
    if (sub_it != subscriber_map_.end()) {
      sub_it->second.message_handler_tool->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    // Register type with participant
    type.register_type(dds_bus_.GetParticipant());

    if (is_ros2_type && options_.ros2_compatible) {
      MW_INFO("ROS2 compatible mode: ibmw topic '{}' -> DDS topic '{}', DDS type: '{}'.",
                 info.topic_name, dds_topic_name, type.get_type_name());
    }

    // Get or create topic
    auto* topic = GetOrCreateTopic(
        dds_bus_.GetParticipant(), dds_topic_name, type.get_type_name(), topic_map_);

    if (topic == nullptr) {
      MW_ERROR("Failed to create DDS topic: {}.", dds_topic_name);
      return false;
    }

    // Get QoS
    auto reader_qos = GetReaderQos(info.topic_name, dds_topic_name);

    // Reader-side DataSharing gate -- mirror the writer-side decision.
    if (options_.enable_data_sharing) {
      reader_qos.data_sharing().automatic();
      MW_INFO("DataSharing enabled for reader (ros2_compatible={}, enable_data_sharing={}).",
                 options_.ros2_compatible, options_.enable_data_sharing);
    }

    // Get executor and loan mode
    auto [executor, loan_mode] = GetExecutorAndLoanMode(info.topic_name);

    // Detect plain type
    bool is_plain = COMPAT_IS_PLAIN(type);
    uint32_t type_size = static_cast<uint32_t>(COMPAT_TYPE_SIZE(type));

    const bool use_plain_loan_receive = is_plain && !is_ros2_type;

    MW_INFO("Subscriber registered: topic='{}', dds_topic='{}', is_plain={}, type_size={} bytes, "
                "loan_mode={}, receive_path={}. ",
                info.topic_name, dds_topic_name, is_plain, type_size,
               LoanModeToString(loan_mode),
               use_plain_loan_receive ? "plain-loan" : "typed-object");

    // Create subscribe tool
    auto message_handler_tool = std::make_shared<runtime::core::transport::MessageHandlerTool>();
    message_handler_tool->AddMessageHandlerWrapper(&message_handler);

    // Create listener
    auto listener = std::make_unique<DdsDataReaderListener>(
        message_handler_tool,
        message_handler,
        use_plain_loan_receive,
        executor,
        loan_mode);

    // Create DataReader with listener
    auto* reader = subscriber_->create_datareader(topic, reader_qos, listener.get());

    if (reader == nullptr) {
      MW_ERROR("Failed to create DataReader for topic: {}.", dds_topic_name);
      return false;
    }

    // Store subscriber info
    SubscriberInfo sub_info;
    sub_info.topic = topic;
    sub_info.reader = reader;
    sub_info.message_handler_tool = std::move(message_handler_tool);
    sub_info.listener = std::move(listener);
    sub_info.executor = executor;
    sub_info.loan_mode = loan_mode;

    subscriber_map_[dds_topic_name] = std::move(sub_info);
    dds_topic_to_ibmw_topic_[dds_topic_name] = info.topic_name;

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("Subscribe failed: {}.", e.what());
    return false;
  }
}

runtime::core::transport::SerializedSubscriberHandle
DdsTransportDriver::AttachSerializedSubscriber(
    const runtime::core::transport::SerializedSubscriberOptions& options,
    runtime::core::transport::SerializedMessageCallback callback) noexcept {
  try {
    MW_REQUIRE(
        state_ == State::kInit || state_ == State::kStart,
        "Serialized subscription requires an initialized DDS driver.");
    MW_REQUIRE(callback, "Serialized subscriber callback cannot be empty.");
    const auto& descriptor = options.topic;
    MW_REQUIRE(!descriptor.topic.empty(), "DDS topic cannot be empty.");
    const bool is_ros2_type = ROS2TypeHelper::IsROS2Type(descriptor.msg_type);
    MW_REQUIRE(
        descriptor.msg_type.starts_with("dds:") || is_ros2_type,
        "Raw DDS subscription requires a 'dds:' or 'ros2:' message type, got "
        "'{}'.",
        descriptor.msg_type);
    MW_REQUIRE(
        descriptor.serialization_type == "cdr" ||
            (is_ros2_type && descriptor.serialization_type == "ros2"),
        "Raw DDS subscription only supports 'cdr' for DDS or 'ros2' CDR for "
        "ROS2, got '{}'.",
        descriptor.serialization_type);

    const std::string dds_type_name =
        is_ros2_type
            ? ROS2TypeHelper::ConvertToDdsTypeName(
                  ROS2TypeHelper::ExtractROS2TypeName(descriptor.msg_type))
            : descriptor.msg_type.substr(4);
    const std::string dds_topic_name =
        GetDdsTopicName(descriptor.topic, descriptor.msg_type);
    eprosima::fastdds::dds::TypeSupport type(
        new RawCdrTopicDataType(dds_type_name));
    type.register_type(dds_bus_.GetParticipant());

    auto* topic = GetOrCreateTopic(
        dds_bus_.GetParticipant(), dds_topic_name, type.get_type_name(),
        topic_map_);
    MW_REQUIRE(topic != nullptr, "Failed to create DDS topic '{}'.", dds_topic_name);

    auto listener = std::make_unique<SerializedDataReaderListener>(
        descriptor, std::move(callback));
    auto reader_qos = GetReaderQos(descriptor.topic, dds_topic_name);
    auto* reader = subscriber_->create_datareader(
        topic, reader_qos, listener.get());
    MW_REQUIRE(
        reader != nullptr,
        "Failed to create raw DDS DataReader for '{}'.", dds_topic_name);

    const uint64_t handle =
        next_serialized_subscriber_handle_.fetch_add(1);
    {
      std::lock_guard lock(serialized_subscriber_mutex_);
      serialized_subscriber_map_.emplace(
          handle,
          SerializedSubscriberInfo{
              .topic = topic,
              .reader = reader,
              .native_type_support = std::move(type),
              .listener = std::move(listener)});
    }
    MW_INFO(
        "Raw DDS subscriber registered: ibmw_topic='{}', dds_topic='{}', "
        "dds_type='{}'.",
        descriptor.topic, dds_topic_name, dds_type_name);
    return {.value = handle};
  } catch (const std::exception& error) {
    MW_ERROR("AttachSerializedSubscriber failed: {}", error.what());
    return {};
  }
}

bool DdsTransportDriver::DetachSerializedSubscriber(
    runtime::core::transport::SerializedSubscriberHandle handle) noexcept {
  try {
    std::lock_guard lock(serialized_subscriber_mutex_);
    const auto found = serialized_subscriber_map_.find(handle.value);
    if (found == serialized_subscriber_map_.end()) {
      return false;
    }
    if (found->second.reader) {
      found->second.reader->set_listener(nullptr);
      const auto result = subscriber_->delete_datareader(found->second.reader);
      if (result != compat_dds::RETCODE_OK) {
        MW_ERROR("Failed to delete raw DDS DataReader.");
        return false;
      }
    }
    serialized_subscriber_map_.erase(found);
    return true;
  } catch (const std::exception& error) {
    MW_ERROR("DetachSerializedSubscriber failed: {}", error.what());
    return false;
  }
}

runtime::core::transport::SerializedPublisherHandle
DdsTransportDriver::RegisterSerializedPublisher(
    const runtime::core::transport::SerializedPublisherOptions&) noexcept {
  return {};
}

bool DdsTransportDriver::SendSerialized(
    runtime::core::transport::SerializedPublisherHandle,
    const runtime::core::transport::SerializedMessage&) noexcept {
  return false;
}

// ---------------------------------------------------------------------------
// Publish
// ---------------------------------------------------------------------------

bool DdsTransportDriver::IsPublishProfileEnabled() noexcept {
  const char* value = std::getenv("IBMW_DDS_PUBLISH_PROFILE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void DdsTransportDriver::LogPublishProfileSnapshot(
    const std::shared_ptr<PublishProfileStats>& stats,
    std::string_view reason,
    std::string_view dds_topic_name) {
  if (!stats) {
    return;
  }

  const uint64_t count = stats->nonplain_write_count.load();
  const uint64_t total_ns = stats->nonplain_write_total_ns.load();
  const uint64_t max_ns = stats->nonplain_write_max_ns.load();
  MW_INFO(
      "DDS publish profile [{}]: topic={} nonplain_write_count={} "
      "nonplain_write_avg_ns={} nonplain_write_max_ns={} nonplain_write_failures={}",
      reason,
      dds_topic_name,
      count,
      count == 0 ? 0 : total_ns / count,
      max_ns,
      stats->nonplain_write_failures.load());
}

void DdsTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    if (state_ != State::kStart) {
      MW_WARN("Publish called when state is not 'Start'.");
      return;
    }

    const auto& info = msg_wrapper.info;

    std::string dds_topic_name = GetDdsTopicName(info.topic_name, info.msg_type);

    auto it = publisher_map_.find(dds_topic_name);
    if (it == publisher_map_.end()) {
      MW_WARN("Publisher not found for topic: {}.", dds_topic_name);
      return;
    }

    auto& pub_info = it->second;

    if (pub_info.is_plain) {
      SendPlainType(msg_wrapper, pub_info, dds_topic_name);
    } else {
      SendNonPlainType(msg_wrapper, pub_info, dds_topic_name);
    }

  } catch (const std::exception& e) {
    MW_ERROR("Publish failed: {}.", e.what());
  }
}

// ---------------------------------------------------------------------------
// SendPlainType - zero-copy via loan_sample + memcpy (1-copy)
// ---------------------------------------------------------------------------

void DdsTransportDriver::SendPlainType(
    runtime::core::transport::MessageFrame& msg_wrapper,
    PublisherInfo& pub_info,
    const std::string& dds_topic_name) {

  MW_TRACE("Publishing plain type to topic: {}.", dds_topic_name);

  // Ensure msg_ptr is available
  runtime::core::transport::CheckMsg(msg_wrapper);
  const void* msg_ptr = msg_wrapper.msg_ptr;

  void* loaned_sample = nullptr;

  auto loan_ret = pub_info.writer->loan_sample(loaned_sample);

  if (loan_ret == compat_dds::RETCODE_OK && loaned_sample != nullptr) {
    pub_info.stats.loan_success++;

    try {
      // Plain type: direct memcpy to loaned (shared memory) region
      constexpr size_t kCdrHeaderSize = 4;
      size_t data_size = pub_info.type_size;
      if (data_size >= kCdrHeaderSize) {
        data_size -= kCdrHeaderSize;
      }

      std::memcpy(loaned_sample, msg_ptr, data_size);

      auto write_ret = pub_info.writer->write(loaned_sample);

      if (COMPAT_WRITE_SUCCESS(write_ret)) {
        pub_info.stats.zero_copy_publish++;
        MW_TRACE("Published plain type via loan_sample (1-copy) to topic: {}.", dds_topic_name);
      } else {
        pub_info.stats.fallback_publish++;
        MW_WARN("Failed to write loaned sample for plain type, topic: {}.", dds_topic_name);
        pub_info.writer->discard_loan(loaned_sample);
      }
    } catch (const std::exception& e) {
      MW_ERROR("Exception during plain type publish: {}.", e.what());
      pub_info.writer->discard_loan(loaned_sample);
      pub_info.stats.fallback_publish++;
    }

  } else {
    // Fallback: loan failed, use regular write (2-copy)
    pub_info.stats.loan_failure++;
    pub_info.stats.fallback_publish++;
    MW_TRACE("loan_sample failed for plain type, using fallback write for topic: {}.",
               dds_topic_name);
    pub_info.writer->write(const_cast<void*>(msg_ptr));
  }
}

// ---------------------------------------------------------------------------
// SendNonPlainType - regular write for constructed C++ objects
// ---------------------------------------------------------------------------

void DdsTransportDriver::SendNonPlainType(
    runtime::core::transport::MessageFrame& msg_wrapper,
    PublisherInfo& pub_info,
    const std::string& dds_topic_name) {

  MW_TRACE("Publishing non-plain type to topic: {}.", dds_topic_name);

  // Ensure msg_ptr is available
  runtime::core::transport::CheckMsg(msg_wrapper);
  const void* msg_ptr = msg_wrapper.msg_ptr;

  // Non-plain types (std::string/std::vector-backed ROS2 messages, etc.) must
  // be passed to FastDDS as constructed C++ objects.  DataWriter::write() will
  // serialize via TopicDataType::serialize().  Do not loan raw memory and then
  // deserialize into it: heap-owning fields would be unconstructed/invalid.
  const auto& profile_stats = pub_info.publish_profile_stats;
  const uint64_t write_start_ns =
      profile_stats ? DdsLoanedAsyncProfileStats::NowNs() : 0;
  auto write_ret = pub_info.writer->write(
      const_cast<void*>(msg_ptr), eprosima::fastdds::dds::HANDLE_NIL);
  if (profile_stats) {
    const uint64_t write_ns = DdsLoanedAsyncProfileStats::NowNs() - write_start_ns;
    profile_stats->nonplain_write_count.fetch_add(1, std::memory_order_relaxed);
    profile_stats->nonplain_write_total_ns.fetch_add(write_ns, std::memory_order_relaxed);
    DdsLoanedAsyncProfileStats::UpdateMax(profile_stats->nonplain_write_max_ns, write_ns);
  }
  if (COMPAT_WRITE_SUCCESS(write_ret)) {
    pub_info.stats.fallback_publish++;
    MW_TRACE("Published non-plain type via regular write to topic: {}.",
             dds_topic_name);
  } else {
    if (profile_stats) {
      profile_stats->nonplain_write_failures.fetch_add(1, std::memory_order_relaxed);
    }
    pub_info.stats.fallback_publish++;
    MW_WARN("Failed to write non-plain type to topic: {}, ret={}",
            dds_topic_name, CompatWriteResultString(write_ret));
  }
}

// ---------------------------------------------------------------------------
// DdsDataReaderListener::on_data_available
// ---------------------------------------------------------------------------

void DdsTransportDriver::DdsDataReaderListener::on_data_available(
    eprosima::fastdds::dds::DataReader* reader) {
  try {
    if (!is_plain_ && nonplain_profile_stats_) {
      const uint64_t entry_ns = DdsLoanedAsyncProfileStats::NowNs();
      nonplain_profile_stats_->on_data_available_count.fetch_add(
          1,
          std::memory_order_relaxed);
      const uint64_t prev_entry_ns =
          nonplain_profile_stats_->on_data_available_last_ns.exchange(
              entry_ns,
              std::memory_order_relaxed);
      if (prev_entry_ns != 0 && entry_ns >= prev_entry_ns) {
        const uint64_t interarrival_ns = entry_ns - prev_entry_ns;
        nonplain_profile_stats_->on_data_available_interarrival_count.fetch_add(
            1,
            std::memory_order_relaxed);
        nonplain_profile_stats_->on_data_available_interarrival_total_ns.fetch_add(
            interarrival_ns,
            std::memory_order_relaxed);
        DdsLoanedAsyncProfileStats::UpdateMax(
            nonplain_profile_stats_->on_data_available_interarrival_max_ns,
            interarrival_ns);
        DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
            nonplain_profile_stats_->on_data_available_interarrival_buckets,
            interarrival_ns);
      }
    }
    if (is_plain_) {
      HandlePlainType(reader);
    } else {
      HandleNonPlainType(reader);
    }
  } catch (const std::exception& e) {
    MW_ERROR("on_data_available failed: {}.", e.what());
  }
}

void DdsTransportDriver::DdsDataReaderListener::on_requested_deadline_missed(
    eprosima::fastdds::dds::DataReader* /*reader*/,
    const fastdds_compat::RequestedDeadlineMissedStatus& status) {
  if (!IsReaderStatusProfileEnabled()) return;
  MW_WARN(
      "DDS reader status: requested_deadline_missed topic='{}' total_count={} "
      "total_count_change={}",
      message_handler_.info.topic_name,
      status.total_count,
      status.total_count_change);
}

void DdsTransportDriver::DdsDataReaderListener::on_sample_rejected(
    eprosima::fastdds::dds::DataReader* /*reader*/,
    const fastdds_compat::SampleRejectedStatus& status) {
  if (!IsReaderStatusProfileEnabled()) return;
  MW_WARN(
      "DDS reader status: sample_rejected topic='{}' total_count={} "
      "total_count_change={} last_reason={}",
      message_handler_.info.topic_name,
      status.total_count,
      status.total_count_change,
      static_cast<int>(status.last_reason));
}

void DdsTransportDriver::DdsDataReaderListener::on_sample_lost(
    eprosima::fastdds::dds::DataReader* /*reader*/,
    const eprosima::fastdds::dds::SampleLostStatus& status) {
  if (!IsReaderStatusProfileEnabled()) return;
  MW_WARN(
      "DDS reader status: sample_lost topic='{}' total_count={} total_count_change={}",
      message_handler_.info.topic_name,
      status.total_count,
      status.total_count_change);
}

bool DdsTransportDriver::DdsDataReaderListener::WaitForAsyncCallbacks(
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(async_state_->mutex);
  return async_state_->cv.wait_for(lock, timeout, [state = async_state_] {
    return state->pending.load(std::memory_order_acquire) == 0;
  });
}

void DdsTransportDriver::DdsDataReaderListener::StopReclaimerAndDrain() {
  auto reclaimer = loan_reclaimer_;
  if (reclaimer) {
    reclaimer->StopAndDrain();
    loan_reclaimer_.reset();
  }
  LogAsyncProfileOnShutdown("shutdown");
  LogNonPlainProfileSnapshot(
      nonplain_profile_stats_,
      "shutdown",
      static_cast<uint64_t>(async_state_->pending.load(std::memory_order_acquire)));
}

void DdsTransportDriver::DdsDataReaderListener::FinishAsyncCallback(
    const std::shared_ptr<AsyncCallbackState>& state) {
  if (state->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->cv.notify_all();
  }
}

void DdsTransportDriver::DdsDataReaderListener::IncrementAsyncPending(
    const std::shared_ptr<AsyncCallbackState>& state,
    const std::shared_ptr<DdsLoanedAsyncProfileStats>& stats) {
  const int pending = state->pending.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (stats) {
    DdsLoanedAsyncProfileStats::UpdateMax(
        stats->pending_high_water,
        static_cast<uint64_t>(pending));
  }
}

bool DdsTransportDriver::DdsDataReaderListener::IsAsyncProfileEnabled() noexcept {
  const char* value = std::getenv("IBMW_DDS_LOANED_ASYNC_PROFILE");
  return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
}

bool DdsTransportDriver::DdsDataReaderListener::IsReaderStatusProfileEnabled() noexcept {
  const char* value = std::getenv("IBMW_DDS_READER_STATUS_PROFILE");
  return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
}

bool DdsTransportDriver::DdsDataReaderListener::IsNonPlainProfileEnabled() noexcept {
  const char* value = std::getenv("IBMW_DDS_NONPLAIN_PROFILE");
  return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
}

void DdsTransportDriver::DdsDataReaderListener::LogNonPlainProfileSnapshot(
    const std::shared_ptr<NonPlainProfileStats>& stats,
    std::string_view reason,
    uint64_t pending_current) {
  if (!stats) return;

  const auto avg = [](uint64_t total, uint64_t count) -> uint64_t {
    return count == 0 ? 0 : total / count;
  };

  const uint64_t create_count = stats->create_count.load(std::memory_order_relaxed);
  const uint64_t take_count = stats->take_count.load(std::memory_order_relaxed);
  const uint64_t oda_count = stats->on_data_available_count.load(std::memory_order_relaxed);
  const uint64_t oda_interarrival_count =
      stats->on_data_available_interarrival_count.load(std::memory_order_relaxed);
  const uint64_t enqueue_count = stats->dispatch_enqueue_count.load(std::memory_order_relaxed);
  const uint64_t wakeup_count = stats->dispatch_wakeup_count.load(std::memory_order_relaxed);
  const uint64_t dispatch_count = stats->dispatch_count.load(std::memory_order_relaxed);
  if (create_count == 0 && take_count == 0 && dispatch_count == 0) return;

  MW_INFO(
      "DDS nonplain profile [{}]: on_data_available_count={} "
      "on_data_available_interarrival_avg_ns={} "
      "on_data_available_interarrival_max_ns={} "
      "on_data_available_interarrival_p99_bucket_ns={} "
      "create_count={} create_avg_ns={} create_max_ns={} "
      "take_count={} take_avg_ns={} take_max_ns={} take_p99_bucket_ns={} "
      "valid_samples={} dispatch_count={} dispatch_failures={} pending_current={} "
      "enqueue_avg_ns={} enqueue_max_ns={} enqueue_p99_bucket_ns={} "
      "wakeup_avg_ns={} wakeup_max_ns={} wakeup_p99_bucket_ns={} "
      "callback_before_dispatch_return={} callback_avg_ns={} callback_max_ns={} "
      "callback_p99_bucket_ns={}",
      reason,
      oda_count,
      avg(
          stats->on_data_available_interarrival_total_ns.load(std::memory_order_relaxed),
          oda_interarrival_count),
      stats->on_data_available_interarrival_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->on_data_available_interarrival_buckets,
          oda_interarrival_count),
      create_count,
      avg(stats->create_total_ns.load(std::memory_order_relaxed), create_count),
      stats->create_max_ns.load(std::memory_order_relaxed),
      take_count,
      avg(stats->take_total_ns.load(std::memory_order_relaxed), take_count),
      stats->take_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(stats->take_buckets, take_count),
      stats->valid_samples.load(std::memory_order_relaxed),
      dispatch_count,
      stats->dispatch_failures.load(std::memory_order_relaxed),
      pending_current,
      avg(stats->dispatch_enqueue_total_ns.load(std::memory_order_relaxed), enqueue_count),
      stats->dispatch_enqueue_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->dispatch_enqueue_buckets, enqueue_count),
      avg(stats->dispatch_wakeup_total_ns.load(std::memory_order_relaxed), wakeup_count),
      stats->dispatch_wakeup_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->dispatch_wakeup_buckets, wakeup_count),
      stats->callback_before_dispatch_return.load(std::memory_order_relaxed),
      avg(stats->callback_total_ns.load(std::memory_order_relaxed), dispatch_count),
      stats->callback_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->callback_buckets, dispatch_count));
}

void DdsTransportDriver::DdsDataReaderListener::LogAsyncProfileOnShutdown(
    std::string_view reason) const {
  const uint64_t pending_current = static_cast<uint64_t>(
      async_state_->pending.load(std::memory_order_acquire));
  LogAsyncProfileSnapshot(async_profile_stats_, reason, pending_current);
}

void DdsTransportDriver::DdsDataReaderListener::LogAsyncProfileSnapshot(
    const std::shared_ptr<DdsLoanedAsyncProfileStats>& stats,
    std::string_view reason,
    uint64_t pending_current) {
  if (!stats) return;

  const uint64_t dispatch_count = stats->dispatch_count.load(std::memory_order_relaxed);
  const uint64_t batches = stats->batches.load(std::memory_order_relaxed);

  const auto avg = [](uint64_t total, uint64_t count) -> uint64_t {
    return count == 0 ? 0 : total / count;
  };

  const uint64_t return_count = stats->return_count.load(std::memory_order_relaxed);
  const uint64_t reclaimer_enqueues = stats->reclaimer_enqueues.load(std::memory_order_relaxed);
  const uint64_t enqueue_count = stats->dispatch_enqueue_count.load(std::memory_order_relaxed);
  const uint64_t wakeup_count = stats->dispatch_wakeup_count.load(std::memory_order_relaxed);

  MW_INFO(
      "DDS loaned async profile [{}]: dispatch_count={} dispatch_failures={} "
      "pending_high_water={} pending_current={} "
      "queue_avg_ns={} queue_max_ns={} enqueue_avg_ns={} enqueue_max_ns={} "
      "enqueue_p99_bucket_ns={} wakeup_avg_ns={} wakeup_max_ns={} "
      "wakeup_p99_bucket_ns={} callback_before_dispatch_return={} "
      "callback_avg_ns={} callback_max_ns={} "
      "batches={} valid_samples={} sample_refs={} batch_holds={} "
      "batch_lifetime_avg_ns={} batch_lifetime_max_ns={} reclaimer_enqueues={} "
      "return_count={} return_avg_ns={} return_max_ns={}",
      reason,
      dispatch_count,
      stats->dispatch_failures.load(std::memory_order_relaxed),
      stats->pending_high_water.load(std::memory_order_relaxed),
      pending_current,
      avg(stats->dispatch_queue_total_ns.load(std::memory_order_relaxed), dispatch_count),
      stats->dispatch_queue_max_ns.load(std::memory_order_relaxed),
      avg(stats->dispatch_enqueue_total_ns.load(std::memory_order_relaxed), enqueue_count),
      stats->dispatch_enqueue_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->dispatch_enqueue_buckets, enqueue_count),
      avg(stats->dispatch_wakeup_total_ns.load(std::memory_order_relaxed), wakeup_count),
      stats->dispatch_wakeup_max_ns.load(std::memory_order_relaxed),
      DdsLoanedAsyncProfileStats::EstimateP99BucketNs(
          stats->dispatch_wakeup_buckets, wakeup_count),
      stats->callback_before_dispatch_return.load(std::memory_order_relaxed),
      avg(stats->callback_total_ns.load(std::memory_order_relaxed), dispatch_count),
      stats->callback_max_ns.load(std::memory_order_relaxed),
      batches,
      stats->valid_samples.load(std::memory_order_relaxed),
      stats->sample_refs.load(std::memory_order_relaxed),
      stats->batch_holds.load(std::memory_order_relaxed),
      avg(stats->batch_lifetime_total_ns.load(std::memory_order_relaxed), reclaimer_enqueues),
      stats->batch_lifetime_max_ns.load(std::memory_order_relaxed),
      reclaimer_enqueues,
      return_count,
      avg(stats->return_total_ns.load(std::memory_order_relaxed), return_count),
      stats->return_max_ns.load(std::memory_order_relaxed));
}

void DdsTransportDriver::DdsDataReaderListener::HandlePlainType(
    eprosima::fastdds::dds::DataReader* reader) {

  // Create loan batch
  std::shared_ptr<DdsLoanReclaimer> reclaimer;
  {
    std::lock_guard<std::mutex> reader_lock(*reader_mutex_);
    if (!loan_reclaimer_) {
      loan_reclaimer_ = std::make_shared<DdsLoanReclaimer>(reader, reader_mutex_);
    }
    reclaimer = loan_reclaimer_;
  }
  auto loan_batch = DdsLoanBus::CreateLoanBatch(
      std::move(reclaimer), "", async_profile_stats_);

  auto ret = loan_batch->TakeLoan();
  if (ret != kRetcodeOk) {
    return;
  }

  const bool async_dispatch = (loan_mode_ == LoanMode::ASYNC && executor_);
  std::shared_ptr<void> async_batch_pending_hold;
  if (async_dispatch) {
    IncrementAsyncPending(async_state_, async_profile_stats_);
    async_batch_pending_hold = std::shared_ptr<void>(
        nullptr,
        [state = async_state_](void*) {
          FinishAsyncCallback(state);
        });
  }

  size_t sample_count = loan_batch->GetSampleCount();
  auto batch_hold = loan_batch->CreateBatchHold(loan_batch);
  if (!batch_hold) {
    return;
  }

  for (size_t i = 0; i < sample_count; ++i) {
    if (!loan_batch->IsValidSample(i)) {
      continue;
    }

    // Create shared_ptr that holds a reference to the loan batch (zero-copy)
    auto msg_shared_ptr = loan_batch->CreateSharedPtr(i, loan_batch);
    if (!msg_shared_ptr) {
      continue;
    }

    // Create context
    auto ctx_ptr = std::make_shared<ibmw::transport::Context>(
        ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

    // Route to executor or call synchronously. Fan-out is centralized through
    // MessageHandlerTool so same-topic subscribers all receive the sample.
    if (async_dispatch) {
      const auto profile_stats = async_profile_stats_;
      IncrementAsyncPending(async_state_, profile_stats);
      const uint64_t dispatch_submit_ns =
          profile_stats ? DdsLoanedAsyncProfileStats::NowNs() : 0;
      auto dispatch_return_ns = profile_stats ? std::make_shared<std::atomic<uint64_t>>(0) : nullptr;
      try {
        executor_.Dispatch([ctx_ptr, msg_shared_ptr,
                            handler_tool = message_handler_tool_,
                            message_handler = &message_handler_,
                            state = async_state_,
                            profile_stats,
                            dispatch_submit_ns,
                            dispatch_return_ns]() mutable {
          uint64_t callback_start_ns = 0;
          uint64_t current_dispatch_count = 0;
          if (profile_stats) {
            callback_start_ns = DdsLoanedAsyncProfileStats::NowNs();
            const uint64_t queue_ns = callback_start_ns - dispatch_submit_ns;
            const uint64_t dispatch_return_value =
                dispatch_return_ns ? dispatch_return_ns->load(std::memory_order_acquire) : 0;
            current_dispatch_count =
                profile_stats->dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
            profile_stats->dispatch_queue_total_ns.fetch_add(queue_ns, std::memory_order_relaxed);
            DdsLoanedAsyncProfileStats::UpdateMax(
                profile_stats->dispatch_queue_max_ns, queue_ns);
            if (dispatch_return_value != 0 && callback_start_ns >= dispatch_return_value) {
              const uint64_t wakeup_ns = callback_start_ns - dispatch_return_value;
              profile_stats->dispatch_wakeup_count.fetch_add(1, std::memory_order_relaxed);
              profile_stats->dispatch_wakeup_total_ns.fetch_add(
                  wakeup_ns, std::memory_order_relaxed);
              DdsLoanedAsyncProfileStats::UpdateMax(
                  profile_stats->dispatch_wakeup_max_ns, wakeup_ns);
              DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
                  profile_stats->dispatch_wakeup_buckets, wakeup_ns);
            } else {
              profile_stats->callback_before_dispatch_return.fetch_add(
                  1, std::memory_order_relaxed);
            }
          }
          try {
            handler_tool->DoMessageCallback(ctx_ptr, *message_handler, msg_shared_ptr);
          } catch (...) {
            if (profile_stats) {
              const uint64_t callback_ns =
                  DdsLoanedAsyncProfileStats::NowNs() - callback_start_ns;
              profile_stats->callback_total_ns.fetch_add(callback_ns, std::memory_order_relaxed);
              DdsLoanedAsyncProfileStats::UpdateMax(
                  profile_stats->callback_max_ns, callback_ns);
            }
            msg_shared_ptr.reset();
            FinishAsyncCallback(state);
            throw;
          }
          if (profile_stats) {
            const uint64_t callback_ns =
                DdsLoanedAsyncProfileStats::NowNs() - callback_start_ns;
            profile_stats->callback_total_ns.fetch_add(callback_ns, std::memory_order_relaxed);
            DdsLoanedAsyncProfileStats::UpdateMax(
                profile_stats->callback_max_ns, callback_ns);
            if (current_dispatch_count % 256 == 0) {
              const uint64_t pending_current = static_cast<uint64_t>(
                  state->pending.load(std::memory_order_acquire));
              LogAsyncProfileSnapshot(profile_stats, "periodic", pending_current);
            }
          }
          msg_shared_ptr.reset();
          FinishAsyncCallback(state);
        });
        if (profile_stats) {
          const uint64_t dispatch_return_value = DdsLoanedAsyncProfileStats::NowNs();
          dispatch_return_ns->store(dispatch_return_value, std::memory_order_release);
          const uint64_t enqueue_ns = dispatch_return_value - dispatch_submit_ns;
          profile_stats->dispatch_enqueue_count.fetch_add(1, std::memory_order_relaxed);
          profile_stats->dispatch_enqueue_total_ns.fetch_add(
              enqueue_ns, std::memory_order_relaxed);
          DdsLoanedAsyncProfileStats::UpdateMax(
              profile_stats->dispatch_enqueue_max_ns, enqueue_ns);
          DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
              profile_stats->dispatch_enqueue_buckets, enqueue_ns);
        }
      } catch (...) {
        if (profile_stats) {
          profile_stats->dispatch_failures.fetch_add(1, std::memory_order_relaxed);
        }
        FinishAsyncCallback(async_state_);
        throw;
      }
    } else {
      message_handler_tool_->DoMessageCallback(ctx_ptr, message_handler_, msg_shared_ptr);
    }
  }
}

// ---------------------------------------------------------------------------
// HandleNonPlainType - take_next_sample (1-copy)
// ---------------------------------------------------------------------------

void DdsTransportDriver::DdsDataReaderListener::HandleNonPlainType(
    eprosima::fastdds::dds::DataReader* reader) {

  eprosima::fastdds::dds::SampleInfo sample_info;
  const auto profile_stats = nonplain_profile_stats_;

  while (true) {
    // For non-plain native DDS types, use the TypeSupport to create/delete data
    const void* custom_ptr = message_handler_.info.msg_type_support_ref.CustomTypeSupportPtr();
    if (!custom_ptr) {
      MW_ERROR("No custom type support for non-plain type topic='{}' type='{}'.",
               message_handler_.info.topic_name,
               message_handler_.info.msg_type);
      break;
    }

    auto native_ts = *static_cast<const eprosima::fastdds::dds::TypeSupport*>(custom_ptr);
    const uint64_t create_start_ns =
        profile_stats ? DdsLoanedAsyncProfileStats::NowNs() : 0;
    void* dds_msg = native_ts.create_data();
    if (profile_stats) {
      const uint64_t create_ns = DdsLoanedAsyncProfileStats::NowNs() - create_start_ns;
      profile_stats->create_count.fetch_add(1, std::memory_order_relaxed);
      profile_stats->create_total_ns.fetch_add(create_ns, std::memory_order_relaxed);
      DdsLoanedAsyncProfileStats::UpdateMax(profile_stats->create_max_ns, create_ns);
    }
    if (dds_msg == nullptr) {
      MW_ERROR("DDS non-plain create_data returned null topic='{}' type='{}'.",
               message_handler_.info.topic_name,
               message_handler_.info.msg_type);
      break;
    }

    const uint64_t take_start_ns =
        profile_stats ? DdsLoanedAsyncProfileStats::NowNs() : 0;
    auto ret = reader->take_next_sample(dds_msg, &sample_info);
    if (profile_stats) {
      const uint64_t take_ns = DdsLoanedAsyncProfileStats::NowNs() - take_start_ns;
      profile_stats->take_count.fetch_add(1, std::memory_order_relaxed);
      profile_stats->take_total_ns.fetch_add(take_ns, std::memory_order_relaxed);
      DdsLoanedAsyncProfileStats::UpdateMax(profile_stats->take_max_ns, take_ns);
      DdsLoanedAsyncProfileStats::ObserveLatencyBucket(profile_stats->take_buckets, take_ns);
    }
    if (ret != kRetcodeOk) {
      native_ts.delete_data(dds_msg);
      break;
    }

    if (sample_info.valid_data) {
      if (profile_stats) {
        profile_stats->valid_samples.fetch_add(1, std::memory_order_relaxed);
      }
      auto ctx_ptr = std::make_shared<ibmw::transport::Context>(
          ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

      // Wrap in shared_ptr with custom deleter
      auto msg_shared_ptr = std::shared_ptr<void>(
          dds_msg,
          [ts_copy = native_ts](void* ptr) mutable {
            ts_copy.delete_data(ptr);
          });

      if (loan_mode_ == LoanMode::ASYNC && executor_) {
        IncrementAsyncPending(async_state_, async_profile_stats_);
        const uint64_t dispatch_submit_ns =
            profile_stats ? DdsLoanedAsyncProfileStats::NowNs() : 0;
        auto dispatch_return_ns = profile_stats ? std::make_shared<std::atomic<uint64_t>>(0) : nullptr;
        try {
          executor_.Dispatch([ctx_ptr, msg_shared_ptr,
                              handler_tool = message_handler_tool_,
                              message_handler = &message_handler_,
                              state = async_state_,
                              profile_stats,
                              dispatch_submit_ns,
                              dispatch_return_ns]() mutable {
            uint64_t callback_start_ns = 0;
            uint64_t current_dispatch_count = 0;
            if (profile_stats) {
              callback_start_ns = DdsLoanedAsyncProfileStats::NowNs();
              const uint64_t queue_ns = callback_start_ns - dispatch_submit_ns;
              const uint64_t dispatch_return_value =
                  dispatch_return_ns ? dispatch_return_ns->load(std::memory_order_acquire) : 0;
              current_dispatch_count =
                  profile_stats->dispatch_count.fetch_add(1, std::memory_order_relaxed) + 1;
              if (dispatch_return_value != 0 && callback_start_ns >= dispatch_return_value) {
                const uint64_t wakeup_ns = callback_start_ns - dispatch_return_value;
                profile_stats->dispatch_wakeup_count.fetch_add(1, std::memory_order_relaxed);
                profile_stats->dispatch_wakeup_total_ns.fetch_add(
                    wakeup_ns, std::memory_order_relaxed);
                DdsLoanedAsyncProfileStats::UpdateMax(
                    profile_stats->dispatch_wakeup_max_ns, wakeup_ns);
                DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
                    profile_stats->dispatch_wakeup_buckets, wakeup_ns);
              } else {
                profile_stats->callback_before_dispatch_return.fetch_add(
                    1, std::memory_order_relaxed);
              }
            }
            try {
              handler_tool->DoMessageCallback(ctx_ptr, *message_handler, msg_shared_ptr);
            } catch (...) {
              if (profile_stats) {
                const uint64_t callback_ns =
                    DdsLoanedAsyncProfileStats::NowNs() - callback_start_ns;
                profile_stats->callback_total_ns.fetch_add(callback_ns, std::memory_order_relaxed);
                DdsLoanedAsyncProfileStats::UpdateMax(
                    profile_stats->callback_max_ns, callback_ns);
                DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
                    profile_stats->callback_buckets, callback_ns);
              }
              msg_shared_ptr.reset();
              FinishAsyncCallback(state);
              throw;
            }
            if (profile_stats) {
              const uint64_t callback_ns =
                  DdsLoanedAsyncProfileStats::NowNs() - callback_start_ns;
              profile_stats->callback_total_ns.fetch_add(callback_ns, std::memory_order_relaxed);
              DdsLoanedAsyncProfileStats::UpdateMax(
                  profile_stats->callback_max_ns, callback_ns);
              DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
                  profile_stats->callback_buckets, callback_ns);
              if (current_dispatch_count % 1024 == 0) {
                const uint64_t pending_current = static_cast<uint64_t>(
                    state->pending.load(std::memory_order_acquire));
                LogNonPlainProfileSnapshot(profile_stats, "periodic", pending_current);
              }
            }
            msg_shared_ptr.reset();
            FinishAsyncCallback(state);
          });
          if (profile_stats) {
            const uint64_t dispatch_return_value = DdsLoanedAsyncProfileStats::NowNs();
            dispatch_return_ns->store(dispatch_return_value, std::memory_order_release);
            const uint64_t enqueue_ns = dispatch_return_value - dispatch_submit_ns;
            profile_stats->dispatch_enqueue_count.fetch_add(1, std::memory_order_relaxed);
            profile_stats->dispatch_enqueue_total_ns.fetch_add(
                enqueue_ns, std::memory_order_relaxed);
            DdsLoanedAsyncProfileStats::UpdateMax(
                profile_stats->dispatch_enqueue_max_ns, enqueue_ns);
            DdsLoanedAsyncProfileStats::ObserveLatencyBucket(
                profile_stats->dispatch_enqueue_buckets, enqueue_ns);
          }
        } catch (...) {
          if (profile_stats) {
            profile_stats->dispatch_failures.fetch_add(1, std::memory_order_relaxed);
          }
          FinishAsyncCallback(async_state_);
          throw;
        }
      } else {
        message_handler_tool_->DoMessageCallback(ctx_ptr, message_handler_, msg_shared_ptr);
      }
    } else {
      native_ts.delete_data(dds_msg);
    }
  }
}

// ---------------------------------------------------------------------------
// Zero-copy API helpers
// ---------------------------------------------------------------------------

bool DdsTransportDriver::IsPlainType(const std::string& dds_topic_name) const {
  auto it = publisher_map_.find(dds_topic_name);
  if (it != publisher_map_.end()) return it->second.is_plain;
  return false;
}

bool DdsTransportDriver::CanLoanMessages(const std::string& dds_topic_name) const {
  auto it = publisher_map_.find(dds_topic_name);
  if (it == publisher_map_.end()) return false;
  return it->second.is_plain;  // Only plain types support zero-copy loan
}

eprosima::fastdds::dds::DataWriter* DdsTransportDriver::GetDataWriter(
    const std::string& dds_topic_name) {
  auto it = publisher_map_.find(dds_topic_name);
  if (it != publisher_map_.end()) return it->second.writer;
  return nullptr;
}

int DdsTransportDriver::GetPublicationMatchedCount(
    const std::string& dds_topic_name) const {
  auto it = publisher_map_.find(dds_topic_name);
  if (it == publisher_map_.end() || it->second.writer == nullptr) return -1;

  eprosima::fastdds::dds::PublicationMatchedStatus status;
  const auto ret = it->second.writer->get_publication_matched_status(status);
  if (ret != compat_dds::RETCODE_OK) return -1;
  return status.current_count;
}

const ZeroCopyStats* DdsTransportDriver::GetZeroCopyStats(
    const std::string& dds_topic_name) const {
  auto it = publisher_map_.find(dds_topic_name);
  if (it != publisher_map_.end()) return &it->second.stats;
  return nullptr;
}

void DdsTransportDriver::UpdateStats(
    const std::string& dds_topic_name, bool is_zero_copy) {
  auto it = publisher_map_.find(dds_topic_name);
  if (it == publisher_map_.end()) return;
  if (is_zero_copy) {
    it->second.stats.zero_copy_publish++;
  } else {
    it->second.stats.fallback_publish++;
  }
}

const ZeroCopyConfig& DdsTransportDriver::GetZeroCopyConfig(
    const std::string& dds_topic_name) const {
  // Reverse-map to ibmw topic name
  auto rev_it = dds_topic_to_ibmw_topic_.find(dds_topic_name);
  if (rev_it != dds_topic_to_ibmw_topic_.end()) {
    return zero_copy_config_mgr_.GetConfig(rev_it->second);
  }
  return zero_copy_config_mgr_.GetGlobalConfig();
}

// ---------------------------------------------------------------------------
// Phase 7 §7.3 §K-3.6: user-level zero-copy loan fast lane (POD only)
//
// Routes core dispatcher's Loan/PublishLoaned/ReleaseLoaned to FastDDS'
// native loan_sample / write / discard_loan. Restricted to plain (PlainCdr)
// types: CDR encapsulation differs from native struct layout, so non-plain
// types cannot be loaned in place — we'd need a serialize step that defeats
// the purpose. Returning false on non-plain lets the dispatcher try the
// next driver (typically iceoryx for the bench types).
//
// Layout note: FastDDS 2.6 / 3.x loan_sample returns a region of size
// (m_typeSize), where PlainDdsPubSubType<T> sets m_typeSize = sizeof(T) + 4
// (4-byte CDR encapsulation header). The user-visible payload begins at
// offset 0 within the loaned chunk and is sizeof(T) bytes — FastDDS
// internally manages the encapsulation prefix. This matches what
// SendPlainType() does today (memcpy data_size = type_size - 4 to offset 0).
// ---------------------------------------------------------------------------

namespace {
constexpr size_t kPlainCdrEncapHeaderBytes = 4;
}  // namespace

bool DdsTransportDriver::LoanSample(
    const runtime::core::transport::TransportInfo& info,
    size_t requested_size,
    runtime::core::transport::LoanMode mode,
    const ibmw::transport::Context* /*ctx*/,
    runtime::core::transport::LoanedSampleHandle& out_handle) noexcept {
  if (mode != runtime::core::transport::LoanMode::kPodOnly) {
    MW_TRACE("DdsTransportDriver::LoanSample: only kPodOnly supported "
             "(ctx-meta would require re-serialize); returning false.");
    return false;
  }
  try {
    if (state_ != State::kStart) return false;

    std::string dds_topic_name = GetDdsTopicName(info.topic_name, info.msg_type);
    auto it = publisher_map_.find(dds_topic_name);
    if (it == publisher_map_.end()) {
      MW_TRACE("DdsTransportDriver::LoanSample: publisher not found for "
               "topic={} msg_type={} (dds_topic={})",
               info.topic_name, info.msg_type, dds_topic_name);
      return false;
    }
    auto& pub_info = it->second;

    if (!pub_info.is_plain || pub_info.writer == nullptr) {
      MW_TRACE("DdsTransportDriver::LoanSample: topic={} not plain or writer "
               "null; cannot loan.", dds_topic_name);
      return false;
    }

    // FastDDS loan_sample chunk size is fixed at type_size - encap header
    // bytes (the user-visible payload region). Reject oversized requests
    // up front so the user sees a clean failure instead of buffer overrun.
    size_t usable_size = (pub_info.type_size > kPlainCdrEncapHeaderBytes)
                            ? (pub_info.type_size - kPlainCdrEncapHeaderBytes)
                            : 0;
    if (requested_size > usable_size) {
      MW_WARN("DdsTransportDriver::LoanSample: requested_size={} exceeds "
              "PlainCdr usable size={} (type_size={}) for topic={}",
              requested_size, usable_size, pub_info.type_size, dds_topic_name);
      return false;
    }

    void* loaned = nullptr;
    auto ret = pub_info.writer->loan_sample(loaned);
    if (ret != compat_dds::RETCODE_OK || loaned == nullptr) {
      pub_info.stats.loan_failure++;
      return false;
    }
    pub_info.stats.loan_success++;

    out_handle.user_payload_ptr = loaned;
    out_handle.user_payload_size = usable_size;
    out_handle.mode = runtime::core::transport::LoanMode::kPodOnly;
    // driver_token holds the writer pointer so PublishLoaned/ReleaseLoaned
    // can route back without re-doing the publisher_map_ lookup.
    out_handle.driver_token = static_cast<void*>(pub_info.writer);
    out_handle.source_driver = this;  // §K-3.3: dispatcher routing key
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("DdsTransportDriver::LoanSample exception: {}", e.what());
    return false;
  }
}

void DdsTransportDriver::PublishLoaned(
    runtime::core::transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr)
    return;
  auto* writer = static_cast<eprosima::fastdds::dds::DataWriter*>(
      handle.driver_token);
  try {
    auto ret = writer->write(handle.user_payload_ptr);
    if (!COMPAT_WRITE_SUCCESS(ret)) {
      MW_WARN("DdsTransportDriver::PublishLoaned: write failed; discarding loan.");
      writer->discard_loan(handle.user_payload_ptr);
    }
  } catch (const std::exception& e) {
    MW_ERROR("DdsTransportDriver::PublishLoaned exception: {}", e.what());
    // Best-effort recovery: try to release the loan so the chunk doesn't leak.
    try { writer->discard_loan(handle.user_payload_ptr); } catch (...) {}
  }
  handle.user_payload_ptr = nullptr;
  handle.user_payload_size = 0;
  handle.driver_token = nullptr;
  handle.source_driver = nullptr;
}

void DdsTransportDriver::ReleaseLoaned(
    runtime::core::transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr)
    return;
  auto* writer = static_cast<eprosima::fastdds::dds::DataWriter*>(
      handle.driver_token);
  try {
    writer->discard_loan(handle.user_payload_ptr);
  } catch (const std::exception& e) {
    MW_ERROR("DdsTransportDriver::ReleaseLoaned exception: {}", e.what());
  }
  handle.user_payload_ptr = nullptr;
  handle.user_payload_size = 0;
  handle.driver_token = nullptr;
  handle.source_driver = nullptr;
}

}  // namespace ibmw::extensions::dds_extension
