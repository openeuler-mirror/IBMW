// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topic_logger_ext/topic_logger_driver.h"
#include "cpp/util/json_type_support.h"
#include "base/string_util.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::topic_logger_extension::TopicLoggerDriver::Options> {
  using Options = ibmw::extensions::topic_logger_extension::TopicLoggerDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["module_filter"] = rhs.module_filter;
    node["interval_ms"] = rhs.interval_ms;
    node["timer_executor_name"] = rhs.timer_executor_name;
    node["topic"] = rhs.topic_name;
    node["max_msg_size"] = rhs.max_msg_size;
    node["max_msg_count"] = rhs.max_msg_count;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["interval_ms"])
      rhs.interval_ms = node["interval_ms"].as<uint32_t>();
    if (node["module_filter"])
      rhs.module_filter = node["module_filter"].as<std::string>();
    if (node["max_msg_size"])
      rhs.max_msg_size = node["max_msg_size"].as<size_t>();
    if (node["max_msg_count"])
      rhs.max_msg_count = node["max_msg_count"].as<size_t>();

    rhs.topic_name = node["topic"].as<std::string>();
    rhs.timer_executor_name = node["timer_executor_name"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::topic_logger_extension {

void TopicLoggerDriver::Initialize(YAML::Node options_node) {
  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  // register timer
  MW_ASSERT(!options_.timer_executor_name.empty(), "Timer executor name is empty.");

  timer_executor_ = get_executor_func_(options_.timer_executor_name);
  MW_ASSERT(timer_executor_, "Invalid timer executor name: {}", options_.timer_executor_name);
  MW_ASSERT(timer_executor_.SupportsTimedDispatch(),
               "Timer executor {} must support timer schedule.", options_.timer_executor_name);

  using LogData = ibmw::protocols::topic_logger::LogData;

  static const std::string kMsgTypeName =
      "json:" + std::string(LogData::kTypeName);

  auto timer_task = [this]() {
    std::unique_ptr<LogData> log_data_ptr;

    {
      std::unique_lock<std::mutex> lck(mutex_);
      // if no log data, then stop timer to avoid unnecessary work
      if (!current_log_data_) [[unlikely]] {
        publish_flag_ = false;
        timer_ptr_->Cancel();
        return;
      }

      // swap
      current_log_data_.swap(log_data_ptr);

      // fill header
      log_data_ptr->header.time_stamp = ibmw::common::util::GetCurTimestampNs();
      log_data_ptr->header.sequence_num = sequence_num_++;
      log_data_ptr->dropped_count = dropped_msg_count_;

      dropped_msg_count_ = 0;
    }

    ibmw::transport::Context ctx;
    ctx.SetSerializationType("json");
    log_publisher_.Send(kMsgTypeName, ctx, static_cast<const void*>(log_data_ptr.get()));
  };

  timer_ptr_ = ibmw::executor::CreateTimer(timer_executor_,
                                           std::chrono::milliseconds(options_.interval_ms),
                                           std::move(timer_task),
                                           false);
  options_node = options_;

  run_flag_.store(true);
}

void TopicLoggerDriver::Log(const runtime::core::logger::LogDataWrapper& log_data_wrapper) noexcept {
  try {
    if (!run_flag_.load()) [[unlikely]] {
      return;
    }

    if (!CheckLog(log_data_wrapper)) [[unlikely]] {
      return;
    }

    using SingleLogData = ibmw::protocols::topic_logger::SingleLogData;
    using LogData = ibmw::protocols::topic_logger::LogData;

    // fill single log data
    SingleLogData single_log_data;
    single_log_data.module_name = std::string(log_data_wrapper.module_name);
    single_log_data.thread_id = log_data_wrapper.thread_id;
    single_log_data.time_point = log_data_wrapper.t.time_since_epoch().count();
    single_log_data.level = static_cast<ibmw::protocols::topic_logger::LogLevel>(log_data_wrapper.lvl);
    single_log_data.line = log_data_wrapper.line;
    single_log_data.file_name = log_data_wrapper.file_name;
    single_log_data.function_name = log_data_wrapper.function_name;

    const auto* log_data_ptr_raw = log_data_wrapper.log_data;
    size_t used_msg_size = common::util::SafeUtf8TruncationLength(log_data_ptr_raw, log_data_wrapper.log_data_size, max_msg_size_);
    single_log_data.message = std::string(log_data_ptr_raw, used_msg_size);

    {
      std::unique_lock<std::mutex> lck(mutex_);
      // if current_log_data_ is null, then create a new one
      if (!current_log_data_) [[unlikely]] {
        current_log_data_ = std::make_unique<LogData>();
      }

      // check current data size
      if (current_log_data_->logs.size() >= options_.max_msg_count) [[unlikely]] {
        dropped_msg_count_++;
        return;
      }

      current_log_data_->logs.emplace_back(std::move(single_log_data));

      // if timer is stop, then reset it
      if (!publish_flag_) [[unlikely]] {
        publish_flag_ = true;
        timer_ptr_->Reset();
      }
    }

  } catch (const std::exception& e) {
    (void)fprintf(stderr, "Log get exception: %s\n", e.what());
  }
}

bool TopicLoggerDriver::CheckLog(const runtime::core::logger::LogDataWrapper& log_data_wrapper) {
  {
    std::shared_lock lock(module_filter_map_mutex_);
    auto find_itr = module_filter_map_.find(log_data_wrapper.module_name);
    if (find_itr != module_filter_map_.end()) {
      return find_itr->second;
    }
  }

  bool if_log = false;

  try {
    if (std::regex_match(
            log_data_wrapper.module_name.begin(),
            log_data_wrapper.module_name.end(),
            std::regex(options_.module_filter, std::regex::ECMAScript))) {
      if_log = true;
    }
  } catch (const std::exception& e) {
    // module_name is a string_view and need not be NUL-terminated.
    (void)fprintf(stderr, "Regex get exception, expr: %s, string: %.*s, exception info: %s\n",
                  options_.module_filter.c_str(),
                  static_cast<int>(log_data_wrapper.module_name.size()),
                  log_data_wrapper.module_name.data(), e.what());
  }

  std::unique_lock lock(module_filter_map_mutex_);
  module_filter_map_.emplace(log_data_wrapper.module_name, if_log);

  return if_log;
}

void TopicLoggerDriver::RegisterLogPublisher() {
  using LogData = ibmw::protocols::topic_logger::LogData;

  log_publisher_ = get_publisher_ref_func_(options_.topic_name);
  MW_ASSERT(log_publisher_, "Failed to get publisher for topic: {}", options_.topic_name);

  bool ret = log_publisher_.RegisterSendType(
      ibmw::json::GetJsonMessageTypeSupport<LogData>());
  MW_ASSERT(ret, "Register publish type failed.");
}

}  // namespace ibmw::extensions::topic_logger_extension
