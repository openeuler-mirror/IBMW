// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/record_action.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dynamiclatch.h"
#include "record_playback_ext/global.h"
#include "record_playback_ext/topic_meta.h"
#include "record_playback_ext/time_math.h"
#include "base/string_util.h"
#include "base/time_util.h"

namespace YAML {

template <>
struct convert<ibmw::extensions::record_playback_extension::RecordAction::Options> {
  using Options = ibmw::extensions::record_playback_extension::RecordAction::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["bag_path"] = rhs.bag_path;

    Node storage_policy = YAML::Node();
    storage_policy["max_bag_size_m"] = rhs.storage_policy.max_bag_size_m;
    storage_policy["max_bag_num"] = rhs.storage_policy.max_bag_num;
    storage_policy["msg_write_interval"] = rhs.storage_policy.msg_write_interval;
    storage_policy["msg_write_interval_time"] = rhs.storage_policy.msg_write_interval_time;
    storage_policy["compression_mode"] = rhs.storage_policy.compression_mode;
    storage_policy["compression_level"] = rhs.storage_policy.compression_level;
    storage_policy["new_folder_daily"] = rhs.storage_policy.new_folder_daily;
    storage_policy["min_free_space_m"] = rhs.storage_policy.min_free_space_m;
    storage_policy["max_folder_num"] = rhs.storage_policy.max_folder_num;
    storage_policy["max_total_size_m"] = rhs.storage_policy.max_total_size_m;

    node["storage_policy"] = storage_policy;

    node["record_enabled"] = rhs.record_enabled;

    node["extra_attributes"] = rhs.extra_attributes;
    node["extra_file_path"] = rhs.extra_file_path;

    if (rhs.mode == Options::Mode::kImd) {
      node["mode"] = "imd";
    } else if (rhs.mode == Options::Mode::kSignal) {
      node["mode"] = "signal";
    }

    node["max_preparation_duration_s"] = rhs.max_preparation_duration_s;
    node["max_preparation_cache_m"] = rhs.max_preparation_cache_m;
    node["executor"] = rhs.executor;

    node["topic_meta_list"] = YAML::Node();
    for (const auto& topic_meta : rhs.topic_meta_list) {
      Node topic_meta_node;
      topic_meta_node["topic"] = topic_meta.topic_name;
      topic_meta_node["msg_type"] = topic_meta.msg_type;
      topic_meta_node["serialization_type"] = topic_meta.serialization_type;
      topic_meta_node["sample_freq"] = topic_meta.sample_freq;
      topic_meta_node["record_enabled"] = topic_meta.record_enabled;
      topic_meta_node["cache_last_msg"] = topic_meta.cache_last_msg;
      node["topic_meta_list"].push_back(topic_meta_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    rhs.bag_path = node["bag_path"].as<std::string>();

    auto mode = ibmw::common::util::StrToLower(node["mode"].as<std::string>());
    if (mode == "imd") {
      rhs.mode = Options::Mode::kImd;
    } else if (mode == "signal") {
      rhs.mode = Options::Mode::kSignal;
    } else {
      throw ibmw::common::util::IbmwException("Invalid record mode: " + mode);
    }

    if (node["record_enabled"])
      rhs.record_enabled = node["record_enabled"].as<bool>();

    if (node["storage_policy"]) {
      Node storage_policy = node["storage_policy"];

      if (storage_policy["max_bag_size_m"])
        rhs.storage_policy.max_bag_size_m = storage_policy["max_bag_size_m"].as<uint32_t>();

      if (storage_policy["max_bag_num"])
        rhs.storage_policy.max_bag_num = storage_policy["max_bag_num"].as<uint32_t>();

      if (storage_policy["msg_write_interval"])
        rhs.storage_policy.msg_write_interval = storage_policy["msg_write_interval"].as<uint32_t>();

      if (storage_policy["msg_write_interval_time"])
        rhs.storage_policy.msg_write_interval_time = storage_policy["msg_write_interval_time"].as<uint32_t>();

      if (storage_policy["compression_mode"]) {
        auto compression_mode = ibmw::common::util::StrToLower(storage_policy["compression_mode"].as<std::string>());
        static const std::unordered_set<std::string> valid_compression_mode_set = {"none", "lz4", "zstd"};
        if (!valid_compression_mode_set.contains(compression_mode)) {
          throw ibmw::common::util::IbmwException("Invalid compression mode: " + compression_mode);
        }
        rhs.storage_policy.compression_mode = compression_mode;
      }

      if (storage_policy["compression_level"]) {
        auto compression_level = storage_policy["compression_level"].as<std::string>();
        static const std::unordered_set<std::string> valid_compression_level_set = {"fastest", "fast", "default", "slow", "slowest"};
        if (!valid_compression_level_set.contains(compression_level)) {
          throw ibmw::common::util::IbmwException("Invalid compression level: " + compression_level);
        }
        rhs.storage_policy.compression_level = compression_level;
      }

      if (storage_policy["new_folder_daily"])
        rhs.storage_policy.new_folder_daily = storage_policy["new_folder_daily"].as<bool>();

      if (storage_policy["min_free_space_m"])
        rhs.storage_policy.min_free_space_m = storage_policy["min_free_space_m"].as<uint32_t>();

      if (storage_policy["max_folder_num"])
        rhs.storage_policy.max_folder_num = storage_policy["max_folder_num"].as<uint32_t>();

      if (storage_policy["max_total_size_m"])
        rhs.storage_policy.max_total_size_m = storage_policy["max_total_size_m"].as<uint32_t>();
    }

    if (node["extra_attributes"] && node["extra_attributes"].IsMap()) {
      rhs.extra_attributes = node["extra_attributes"];
    }
    if (node["extra_file_path"] && node["extra_file_path"].IsSequence()) {
      for (const auto& file_path : node["extra_file_path"]) {
        rhs.extra_file_path.emplace_back(file_path.as<std::string>());
      }
    }
    if (node["max_preparation_duration_s"])
      rhs.max_preparation_duration_s = node["max_preparation_duration_s"].as<uint64_t>();
    if (node["max_preparation_cache_m"])
      rhs.max_preparation_cache_m = node["max_preparation_cache_m"].as<uint32_t>();

    rhs.executor = node["executor"].as<std::string>();

    if (node["topic_meta_list"] && node["topic_meta_list"].IsSequence()) {
      for (const auto& topic_meta_node : node["topic_meta_list"]) {
        auto topic_meta = ibmw::extensions::record_playback_extension::TopicMeta{
            .topic_name = topic_meta_node["topic"].as<std::string>(),
            .msg_type = topic_meta_node["msg_type"].as<std::string>()};

        if (topic_meta_node["serialization_type"])
          topic_meta.serialization_type = topic_meta_node["serialization_type"].as<std::string>();

        if (topic_meta_node["sample_freq"])
          topic_meta.sample_freq = topic_meta_node["sample_freq"].as<double>();

        if (topic_meta_node["record_enabled"])
          topic_meta.record_enabled = topic_meta_node["record_enabled"].as<bool>();

        if (topic_meta_node["cache_last_msg"])
          topic_meta.cache_last_msg = topic_meta_node["cache_last_msg"].as<bool>();

        rhs.topic_meta_list.emplace_back(std::move(topic_meta));
      }
    }

    return true;
  }
};

}  // namespace YAML

namespace ibmw::extensions::record_playback_extension {

namespace {

// Writes a temporary file and renames it over the old one, so a reader or a
// crash sees the previous or the new metadata, never a truncated file. No
// fsync: it could stall the record executor on slow storage.
void WriteMetadataFile(const std::filesystem::path& path, const YAML::Node& node) {
  // No bag folder yet: the metadata is written when the first file opens.
  if (path.empty()) return;
  auto tmp_path = path;
  tmp_path += ".tmp";
  std::ofstream ofs(tmp_path, std::ios::trunc);
  ofs << node;
  ofs.close();
  std::error_code ec;
  if (!ofs) {
    MW_ERROR("Failed to write '{}', '{}' is not updated.", tmp_path.string(), path.string());
    std::filesystem::remove(tmp_path, ec);
    return;
  }
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) MW_ERROR("Failed to replace '{}': {}", path.string(), ec.message());
}

}  // namespace

RecordAction::~RecordAction() noexcept {
  try {
    Shutdown();
  } catch (...) {
  }
}

void RecordAction::Initialize(YAML::Node options) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Local transport driver can only be initialized once.");

  if (options && !options.IsNull())
    options_ = options.as<Options>();

  MW_REQUIRE(
      get_type_support_func_,
      "Get type support function is not set before initialize.");
  MW_REQUIRE(
      get_schema_support_func_,
      "Get schema support function is not set before initialize.");
  task_state_ = std::make_shared<TaskState>();
  task_state_->owner = this;

  // check topic meta
  for (auto& topic_meta : options_.topic_meta_list) {
    // check msg type
    auto type_support_ref = get_type_support_func_(topic_meta.msg_type);
    MW_REQUIRE(type_support_ref,
                            "Can not find type '{}' in any type support pkg!", topic_meta.msg_type);

    // check serialization type
    if (!topic_meta.serialization_type.empty()) {
      bool check_ret = type_support_ref.CheckSerializationTypeSupported(topic_meta.serialization_type);
      const bool has_native_schema = get_schema_support_func_(
          topic_meta.msg_type, topic_meta.serialization_type).has_value();
      MW_REQUIRE(
          check_ret || has_native_schema,
          "Msg type '{}' does not support serialization type '{}' and has "
          "no native Schema Support for it.",
          topic_meta.msg_type, topic_meta.serialization_type);
    } else {
      MW_REQUIRE(
          type_support_ref.SerializationTypesSupportedNum() > 0,
          "Msg type '{}' has no default serialization type; configure one "
          "explicitly.",
          topic_meta.msg_type);
      topic_meta.serialization_type = type_support_ref.DefaultSerializationType();
    }
    // check sample freq
    MW_REQUIRE(
        IsValidSampleFreq(topic_meta.sample_freq),
        "Sample freq must be 0 or a finite value >= {}", kMinSampleFreq);
  }

  for (auto& topic_meta_option : options_.topic_meta_list) {
    ibmw::transport::TopicMetaKey key{
        .topic_name = topic_meta_option.topic_name,
        .msg_type = topic_meta_option.msg_type};

    MW_REQUIRE(
        topic_meta_map_.find(key) == topic_meta_map_.end(),
        "Duplicate topic meta, topic name: {}, msg type: {}.",
        topic_meta_option.topic_name, topic_meta_option.msg_type);

    uint32_t topic_id = metadata_.topics.size();
    TopicMeta topic_meta{
        .id = topic_id,
        .topic_name = topic_meta_option.topic_name,
        .msg_type = topic_meta_option.msg_type,
        .serialization_type = topic_meta_option.serialization_type,
        .record_enabled = topic_meta_option.record_enabled,
        .cache_last_msg = topic_meta_option.cache_last_msg,
        .sample_freq = topic_meta_option.sample_freq,
    };

    metadata_.topics.emplace_back(topic_meta);
    topic_meta_map_.emplace(key, topic_meta);
  }
  // misc
  // 0 disables splitting like the other storage_policy limits.
  max_bag_size_ = options_.storage_policy.max_bag_size_m == 0
                      ? std::numeric_limits<size_t>::max()
                      : static_cast<size_t>(options_.storage_policy.max_bag_size_m) * 1024 * 1024;
  max_preparation_duration_ns_ = SecondsToNsSaturated(options_.max_preparation_duration_s);
  max_preparation_cache_bytes_ = static_cast<size_t>(options_.max_preparation_cache_m) * 1024 * 1024;

  metadata_.version = kVersion;
  if (options_.extra_attributes && !options_.extra_attributes.IsNull()) {
    metadata_.extra_attributes = options_.extra_attributes;
  } else {
    metadata_.extra_attributes = YAML::Node(YAML::NodeType::Null);
  }

  SetMcapOptions();
  for (auto& topic_meta : metadata_.topics) {
    auto schema = get_schema_support_func_(
        topic_meta.msg_type, topic_meta.serialization_type);
    MW_REQUIRE(
        schema.has_value(),
        "Cannot record topic '{}': Schema Support is missing for type '{}' "
        "and serialization '{}'.",
        topic_meta.topic_name, topic_meta.msg_type,
        topic_meta.serialization_type);
    MW_REQUIRE(
        !schema->schema_name.empty() && !schema->schema_encoding.empty() &&
            !schema->schema_data.empty() && !schema->schema_hash.empty(),
        "Cannot record topic '{}': Schema Support for type '{}' contains an "
        "empty required field.",
        topic_meta.topic_name, topic_meta.msg_type);
    MW_REQUIRE(
        schema->schema_hash ==
            ibmw::util::ComputeSchemaHash(schema->schema_data),
        "Cannot record topic '{}': Schema hash mismatch for type '{}'.",
        topic_meta.topic_name, topic_meta.msg_type);

    const std::string channel_encoding =
        topic_meta.serialization_type == "ros2"
            ? "cdr"
            : topic_meta.serialization_type;
    mcap_info_map_.emplace(
        topic_meta.id,
        McapStruct{
            .schema_name = schema->schema_name,
            .schema_format = schema->schema_encoding,
            .schema_data = schema->schema_data,
            .schema_hash = schema->schema_hash,
            .transport_name = topic_meta.topic_name,
            .transport_format = channel_encoding});
    // init topic runtime info for sample_freq
    topic_runtime_map_[topic_meta.id].last_timestamp = 0;
    topic_runtime_map_[topic_meta.id].sample_interval = SampleIntervalNs(topic_meta.sample_freq);
    topic_runtime_map_[topic_meta.id].record_enabled = topic_meta.record_enabled;
    topic_runtime_map_[topic_meta.id].cache_last_msg = topic_meta.cache_last_msg;
  }

  parent_bag_path_ = std::filesystem::absolute(options_.bag_path);
  if (!(std::filesystem::exists(parent_bag_path_) && std::filesystem::is_directory(parent_bag_path_))) {
    if (!parent_bag_path_.empty()) std::filesystem::create_directories(parent_bag_path_);
  }

  if (options_.mode == Options::Mode::kImd) {
    bool ret = OpenNewFolderToRecord();
    MW_REQUIRE(ret, "Open new folder to record failed.");
  }
  options = options_;
}

void RecordAction::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
  sync_timer_->Reset();
}

void RecordAction::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (sync_timer_) {
    sync_timer_->Cancel();
  }
  if (task_state_) {
    task_state_->StopAndWait();
  }
  // All tracked executor work has drained.  Queued work that has not entered
  // TaskState is rejected by Enter(), so closing directly is safe even when
  // the executor has already been stopped.
  CloseRecord();
}

void RecordAction::InitExecutor(ibmw::executor::ExecutorHandle timer_executor) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_REQUIRE(
      get_executor_func_,
      "Get executor function is not set before initialize.");

  executor_ = get_executor_func_(options_.executor);
  MW_REQUIRE(
      executor_, "Can not get executor {}.", options_.executor);
  MW_REQUIRE(
      executor_.ThreadSafe(), "Record executor {} is not thread safe!", options_.executor);

  auto task_state = task_state_;
  auto timer_task = [task_state]() {
    RecordAction* action = nullptr;
    if (!task_state || !task_state->Enter(action)) {
      return;
    }
    struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
    action->executor_.Dispatch([task_state]() {
      RecordAction* action = nullptr;
      if (!task_state || !task_state->Enter(action)) {
        return;
      }
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      if (action->state_.load() == State::kStart) {
        // A silent topic brings no record that would end the signal record.
        if (action->recording_flag_ && action->stop_record_timestamp_ != 0 &&
            ibmw::common::util::GetCurTimestampNs() > action->stop_record_timestamp_) {
          action->recording_flag_ = false;
          action->CloseRecord();
        }
        action->FlushToDisk();
      }
    });
  };
  executor_for_async_ = timer_executor;

  sync_timer_ = executor::CreateTimer(timer_executor, std::chrono::milliseconds(options_.storage_policy.msg_write_interval_time), std::move(timer_task), false);
}

void RecordAction::FlushToDisk() {
  if (writer_) {
    writer_->closeLastChunk();
    cur_exec_count_ = 0;
  }
}

void RecordAction::RegisterGetExecutorFunc(
    const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_executor_func_ = get_executor_func;
}

void RecordAction::RegisterGetTypeSupportFunc(
    const std::function<ibmw::util::TypeSupportRef(std::string_view)>& get_type_support_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_type_support_func_ = get_type_support_func;
}

void RecordAction::RegisterGetSchemaSupportFunc(
    const std::function<std::optional<ibmw::util::SchemaDescriptor>(
        std::string_view, std::string_view)>& get_schema_support_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_schema_support_func_ = get_schema_support_func;
}

void RecordAction::AddRecord(OneRecord&& record) {
  if (state_.load() != State::kStart) [[unlikely]] {
    return;
  }

  if (options_.mode == Options::Mode::kImd) {
    auto task_state = task_state_;
    executor_.Dispatch([task_state, record{std::move(record)}]() mutable {
      RecordAction* action = nullptr;
      if (!task_state || !task_state->Enter(action)) {
        return;
      }
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      if (action->state_.load() != State::kStart) return;
      auto& runtime_info = action->topic_runtime_map_[record.topic_index];
      if (!action->options_.record_enabled || !runtime_info.record_enabled) return;
      if (runtime_info.sample_interval > 0 && record.log_timestamp >= runtime_info.last_timestamp &&
          record.log_timestamp - runtime_info.last_timestamp < runtime_info.sample_interval) return;
      runtime_info.last_timestamp = record.log_timestamp;
      if (runtime_info.cache_last_msg) runtime_info.last_msg = record;

      action->AddRecordImpl(std::move(record));
    });
  } else if (options_.mode == Options::Mode::kSignal) {
    auto task_state = task_state_;
    executor_.Dispatch([task_state, record{std::move(record)}]() mutable {
      RecordAction* action = nullptr;
      if (!task_state || !task_state->Enter(action)) {
        return;
      }
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      if (action->state_.load() != State::kStart) return;

      auto& runtime_info = action->topic_runtime_map_[record.topic_index];
      if (!action->options_.record_enabled || !runtime_info.record_enabled) {
        return;
      }
      if (runtime_info.sample_interval > 0 &&
          record.log_timestamp >= runtime_info.last_timestamp &&
          record.log_timestamp - runtime_info.last_timestamp <
              runtime_info.sample_interval) {
        return;
      }
      runtime_info.last_timestamp = record.log_timestamp;
      if (runtime_info.cache_last_msg) {
        runtime_info.last_msg = record;
      }

      uint64_t cur_timestamp = record.log_timestamp;

      if (action->recording_flag_) {
        if (action->stop_record_timestamp_ != 0 && cur_timestamp > action->stop_record_timestamp_) {
          action->recording_flag_ = false;
          action->CloseRecord();
        }
      }

      if (action->recording_flag_) {
        action->AddRecordImpl(std::move(record));
      } else {
        action->cur_cache_.emplace_back(std::move(record));
        if (action->max_preparation_cache_bytes_ != 0) [[unlikely]] action->BoundPreparationCache();

        auto cur_cache_begin_timestamp = action->cur_cache_.begin()->log_timestamp;

        if (cur_timestamp >= cur_cache_begin_timestamp &&
            (cur_timestamp - cur_cache_begin_timestamp) >
                action->max_preparation_duration_ns_) {
          action->cur_cache_.swap(action->last_cache_);
          action->cur_cache_.clear();
          action->last_cache_bytes_ = std::exchange(action->cur_cache_bytes_, 0);
        }
      }
    });
  }
}

bool RecordAction::StartSignalRecord(uint64_t preparation_duration_s, uint64_t record_duration_s, std::string& filefolder) {
  MW_REQUIRE(
      state_.load() == State::kStart,
      "Method can only be called when state is 'Start'.");

  if (options_.mode != Options::Mode::kSignal) [[unlikely]] {
    MW_WARN("Cur action mode is not signal mode.");
    return false;
  }

  util::DynamicLatch latch;
  auto error = std::make_shared<std::exception_ptr>();
  auto task_state = task_state_;
  auto task = [task_state, preparation_duration_s, record_duration_s,
               &filefolder, error, &latch]() {
    RecordAction* action = nullptr;
    if (!task_state || !task_state->Enter(action)) {
      latch.CountDown();
      return;
    }
    struct Guard {
      std::shared_ptr<TaskState> state;
      ~Guard() { state->Leave(); }
    } guard{task_state};
    try {
      if (action->state_.load() != State::kStart) [[unlikely]] {
        latch.CountDown();
        return;
      }

      if (action->recording_flag_) {
        MW_WARN("Recording is already in progress");
        latch.CountDown();
        return;
      }

      // Set only once the folder is open: a failed start must not block
      // the next one.
      bool ret = action->OpenNewFolderToRecord();
      MW_REQUIRE(ret, "Open new folder to record failed.");
      action->recording_flag_ = true;
      filefolder = action->real_bag_path_.string();

      uint64_t now = ibmw::common::util::GetCurTimestampNs();

      action->stop_record_timestamp_ = AddSaturated(now, SecondsToNsSaturated(record_duration_s));

      uint64_t start_record_timestamp = SubSaturated(now, SecondsToNsSaturated(preparation_duration_s));

        // Use binary search to locate the starting point for recording in the cache
      size_t last_cache_size = action->last_cache_.size();
      size_t cur_cache_size = action->cur_cache_.size();
      size_t all_cache_size = last_cache_size + cur_cache_size;

      if (all_cache_size == 0) {
        latch.CountDown();
        return;
      }

        auto get_cahce_timestamp = [&](size_t idx) {
        return (idx < last_cache_size)
                   ? action->last_cache_[idx].log_timestamp
                   : action->cur_cache_[idx - last_cache_size].log_timestamp;
        };

        size_t low = 0, high = all_cache_size - 1;

        while (low < high) {
          size_t mid = low + (high - low) / 2;
          if (get_cahce_timestamp(mid) < start_record_timestamp)
            low = mid + 1;
          else
            high = mid;
        }

        // Write cache to db
        if (low < last_cache_size) {
          for (size_t ii = low; ii < last_cache_size; ++ii) {
            action->AddRecordImpl(std::move(action->last_cache_[ii]));
          }

          for (size_t ii = 0; ii < cur_cache_size; ++ii) {
            action->AddRecordImpl(std::move(action->cur_cache_[ii]));
          }
        } else {
          for (size_t ii = low - last_cache_size; ii < cur_cache_size; ++ii) {
            action->AddRecordImpl(std::move(action->cur_cache_[ii]));
          }
        }

        // Clear the cache
      action->last_cache_.clear();
      action->cur_cache_.clear();
      action->last_cache_bytes_ = action->cur_cache_bytes_ = 0;
      if (action->dropped_cache_records_ != 0) {
        MW_WARN("{} records before the start were dropped by max_preparation_cache_m.",
                std::exchange(action->dropped_cache_records_, 0));
      }
    } catch (...) {
      *error = std::current_exception();
    }
    latch.CountDown();
  };
  if (!latch.TryAdd()) return false;
  executor_.DispatchOrRun(std::move(task));
  latch.CloseAndWait();
  if (*error) std::rethrow_exception(*error);
  return true;
}

bool RecordAction::StopSignalRecord() {
  MW_REQUIRE(
      state_.load() == State::kStart,
      "Method can only be called when state is 'Start'.");

  if (options_.mode != Options::Mode::kSignal) [[unlikely]] {
    MW_WARN("Cur action mode is not signal mode.");
    return false;
  }
  auto task_state = task_state_;
  executor_.Dispatch([task_state]() {
    RecordAction* action = nullptr;
    if (!task_state || !task_state->Enter(action)) {
      return;
    }
    struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
    if (action->state_.load() != State::kStart) return;
    if (!action->recording_flag_) [[unlikely]] {
      MW_WARN("The action has not started yet, stop operation is not allowed.");
      return;
    }
    action->recording_flag_ = false;
    action->CloseRecord();
  });

  return true;
}

void RecordAction::UpdateMetadata(std::unordered_map<std::string, std::string>&& kv_pairs) {
  auto task_state = task_state_;
  executor_.Dispatch([task_state, move_kv_pairs = std::move(kv_pairs)] {
    RecordAction* action = nullptr;
    if (!task_state || !task_state->Enter(action)) return;
    struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
    if (action->state_.load() != State::kStart) return;
    if (!action->metadata_.extra_attributes.IsMap()) {
      action->metadata_.extra_attributes = YAML::Node(YAML::NodeType::Map);
    }
    for (const auto& [key, value] : move_kv_pairs) {
      const YAML::Node& attributes = action->metadata_.extra_attributes;
      if (attributes.size() >= kMaxExtraAttributes && !attributes[key]) [[unlikely]] {
        MW_WARN("Metadata already has {} extra attributes, dropping key '{}'.",
                kMaxExtraAttributes, key);
        continue;
      }
      try {
        YAML::Node parsed_node = YAML::Load(value);
        action->metadata_.extra_attributes[key] = parsed_node;
      } catch (...) {
        action->metadata_.extra_attributes[key] = value;
      }
    }
    YAML::Node node;
    node["ibmw_bagfile_information"] = action->metadata_;

    WriteMetadataFile(action->metadata_yaml_file_path_, node);
  });
}

RecordAction::UpdateResult RecordAction::UpdateTopicMetaRecord(
    std::vector<TopicMeta>&& topic_meta_list, std::optional<bool> action_record_enabled) {
  util::DynamicLatch latch;
  auto task_state = task_state_;
  auto result = UpdateResult::kNotRunning;
  if (!latch.TryAdd()) return result;
  executor_.DispatchOrRun([task_state, move_topic_meta_list = std::move(topic_meta_list),
                            action_record_enabled, &result, &latch]() mutable {
    RecordAction* action = nullptr;
    if (task_state && task_state->Enter(action)) {
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      if (action->state_.load() == State::kStart) {
        std::vector<uint32_t> ids;
        ids.reserve(move_topic_meta_list.size());
        for (const auto& topic_meta : move_topic_meta_list) {
          ibmw::transport::TopicMetaKey key{
              .topic_name = topic_meta.topic_name,
              .msg_type = topic_meta.msg_type};
          auto itr = action->topic_meta_map_.find(key);
          if (itr == action->topic_meta_map_.end()) [[unlikely]] {
            MW_WARN("Topic meta not found: {} {}", topic_meta.topic_name, topic_meta.msg_type);
            result = UpdateResult::kTopicNotFound;
            break;
          }
          ids.push_back(itr->second.id);
        }
        if (result != UpdateResult::kTopicNotFound) {
          if (action_record_enabled.has_value()) {
            action->options_.record_enabled = action_record_enabled.value();
          }
          for (size_t i = 0; i < ids.size(); ++i) {
            auto& runtime = action->topic_runtime_map_[ids[i]];
            runtime.record_enabled = move_topic_meta_list[i].record_enabled;
            runtime.sample_interval = SampleIntervalNs(move_topic_meta_list[i].sample_freq);
          }
          result = UpdateResult::kOk;
        }
      }
    }
    latch.CountDown();
  });
  latch.CloseAndWait();
  return result;
}

void RecordAction::GetStatusSnapshot(StatusSnapshot& snapshot, ibmw::util::DynamicLatch& latch) {
  auto task_state = task_state_;
  if (!latch.TryAdd()) return;
  executor_.DispatchOrRun([task_state, &snapshot, &latch]() {
    RecordAction* action = nullptr;
    if (task_state && task_state->Enter(action)) {
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      if (action->state_.load() == State::kStart) {
        snapshot.record_enabled = action->options_.record_enabled;
        snapshot.topic_meta_list = action->metadata_.topics;
        for (auto& topic_meta : snapshot.topic_meta_list) {
          auto itr = action->topic_runtime_map_.find(topic_meta.id);
          if (itr != action->topic_runtime_map_.end()) {
            topic_meta.record_enabled = itr->second.record_enabled;
          }
        }
      }
    }
    latch.CountDown();
  });
}

size_t RecordAction::GetFileSize() const {
  if (cur_mcap_file_path_.empty() || !std::filesystem::exists(cur_mcap_file_path_)) return 0;
  return std::filesystem::file_size(cur_mcap_file_path_);
}

bool RecordAction::IsNewFolderNeeded(uint64_t timestamp) const {
  if (options_.storage_policy.new_folder_daily && !metadata_.files.empty()) {
    uint64_t last_timestamp = metadata_.files.back().start_timestamp;
    auto last_sec = static_cast<time_t>(last_timestamp / 1000000000ULL);
    auto current_sec = static_cast<time_t>(timestamp / 1000000000ULL);
    auto last_tm = ibmw::common::util::TimeT2TmLocal(last_sec);
    auto current_tm = ibmw::common::util::TimeT2TmLocal(current_sec);
    return (last_tm.tm_mday != current_tm.tm_mday ||
            last_tm.tm_mon != current_tm.tm_mon ||
            last_tm.tm_year != current_tm.tm_year);
  }
  return false;
}

void RecordAction::AddRecordImpl(OneRecord&& record) {
  if (write_paused_) [[unlikely]] {
    if (std::chrono::steady_clock::now() < next_space_check_) return;
    // The pause may have spanned midnight.
    if (IsNewFolderNeeded(record.log_timestamp)) {
      OpenNewFolderToRecord();
    } else {
      OpenNewMcapToRecord(record.log_timestamp);
    }
    if (write_paused_) return;
  }

  // try to open a new
  if (!writing_cached_messages_ && cur_data_size_ * estimated_overhead_ >= max_bag_size_) [[unlikely]] {
    size_t original_cur_data_size = cur_data_size_;
    cur_data_size_ = 0;
    // No file (a failed open) says nothing about the overhead: keep the estimate.
    if (const size_t file_size = GetFileSize(); file_size != 0 && original_cur_data_size != 0) {
      estimated_overhead_ = std::max(0.1, static_cast<double>(file_size) / original_cur_data_size);
    }

    if (IsNewFolderNeeded(record.log_timestamp)) [[unlikely]] {
      OpenNewFolderToRecord();
    } else {
      OpenNewMcapToRecord(record.log_timestamp);
    }
    // Out of space or the open failed: no file is open, the record is dropped.
    if (write_paused_) [[unlikely]] return;
  }

  mcap::Message msg{
      .channelId = topic_runtime_map_[record.topic_index].transport_id,
      .sequence = record.pub_sequence,  // Unique per topic only in the single-publisher case.
      .logTime = record.log_timestamp,
      .publishTime = record.pub_timestamp,
  };

  auto data = record.buffer_view_ptr->JoinToString();
  msg.data = reinterpret_cast<const std::byte*>(data.data());
  msg.dataSize = data.size();
  cur_data_size_ += 30 + data.size();

  auto res = writer_->write(msg);
  if (!res.ok()) {
    MW_WARN("Failed to write record to mcap file: {}", res.message);
    return;
  }

  cur_exec_count_++;
  if (options_.storage_policy.msg_write_interval > 0 && cur_exec_count_ > options_.storage_policy.msg_write_interval) [[unlikely]] {
    FlushToDisk();
  }
}

void RecordAction::CloseRecord() {
  if (writer_)
    writer_->close();
}

namespace {

// Upper bounds for everything copied into one bag's extra_file folder: the
// copy is meant for configuration files, not for whole data directories.
constexpr uintmax_t kMaxExtraFileBytes = 256ULL << 20;
constexpr size_t kMaxExtraFileEntries = 10000;

struct ExtraFileEntry {
  std::filesystem::path rel;  // relative to the copied directory
  bool is_dir;
};

// Lists the tree under `root` without following symbolic links; returns
// false (with a warning) if it holds more than `budget` bytes or more than
// kMaxExtraFileEntries entries.
bool CollectExtraFileTree(const std::filesystem::path& root, uintmax_t budget,
                          std::vector<ExtraFileEntry>& entries, uintmax_t& total) {
  for (auto it = std::filesystem::recursive_directory_iterator(root);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (entries.size() >= kMaxExtraFileEntries) {
      MW_WARN("Extra file path '{}' has more than {} entries, skip copying.", root.string(), kMaxExtraFileEntries);
      return false;
    }
    const auto status = it->symlink_status();
    if (std::filesystem::is_symlink(status)) {
      MW_WARN("Extra file '{}' is a symbolic link, skip copying.", it->path().string());
      continue;
    }
    if (std::filesystem::is_directory(status)) {
      entries.push_back({it->path().lexically_relative(root), true});
    } else if (std::filesystem::is_regular_file(status)) {
      total += it->file_size();
      if (total > budget) {
        MW_WARN("Extra file path '{}' exceeds the {} bytes limit, skip copying.", root.string(), budget);
        return false;
      }
      entries.push_back({it->path().lexically_relative(root), false});
    } else {
      MW_WARN("Extra file '{}' is neither regular file nor directory, skip copying.", it->path().string());
    }
  }
  return true;
}

}  // namespace

// `extra_file_dir` is passed by value from the record thread: the copy may
// run in the background while the next bag folder is opened.
void RecordAction::CopyExtraFilePathToNewFolder(const std::filesystem::path& extra_file_dir) const {
  uintmax_t copied = 0;

  for (const auto& file_path_str : options_.extra_file_path) {
    if (file_path_str.empty()) [[unlikely]] {
      MW_WARN("Extra file path is empty, skip copying.");
      continue;
    }

    try {
      // The copy keeps the configured name; if the configured path itself is
      // a link, its target is copied.
      std::filesystem::path configured = std::filesystem::absolute(file_path_str).lexically_normal();
      if (configured.filename().empty()) configured = configured.parent_path();  // trailing separator
      const auto name = configured.filename();
      if (name.empty() || name == "." || name == "..") {
        MW_WARN("Extra file path '{}' has no file name, skip copying.", file_path_str);
        continue;
      }
      const std::filesystem::path src_path = std::filesystem::weakly_canonical(configured);
      if (!std::filesystem::exists(src_path)) {
        MW_WARN("Extra file path '{}' does not exist, skip copying.", src_path.string());
        continue;
      }
      // Each entry gets its own name below extra_file: an entry with the
      // same name as an earlier one is not copied over it.
      const std::filesystem::path dest = extra_file_dir / name;
      if (std::filesystem::exists(std::filesystem::symlink_status(dest))) {
        MW_WARN("Extra file '{}' has already existed, skip copying '{}'.", dest.string(), src_path.string());
        continue;
      }

      if (std::filesystem::is_directory(src_path)) {
        std::vector<ExtraFileEntry> entries;
        uintmax_t total = copied;
        if (!CollectExtraFileTree(src_path, kMaxExtraFileBytes, entries, total)) continue;
        // A failure part way leaves a partial tree (reported below); the
        // budget is charged up front.
        copied = total;
        std::filesystem::create_directories(dest);
        for (const auto& entry : entries) {
          if (entry.is_dir) {
            std::filesystem::create_directories(dest / entry.rel);
          } else if (std::filesystem::is_regular_file(std::filesystem::symlink_status(src_path / entry.rel))) {
            // Re-checked: the entry may have been replaced since it was listed.
            std::filesystem::copy_file(src_path / entry.rel, dest / entry.rel);
          } else {
            MW_WARN("Extra file '{}' changed while copying, skip it.", (src_path / entry.rel).string());
          }
        }
      } else if (std::filesystem::is_regular_file(src_path)) {
        const uintmax_t size = std::filesystem::file_size(src_path);
        if (size > kMaxExtraFileBytes - copied) {
          MW_WARN("Extra file path '{}' exceeds the {} bytes limit, skip copying.", src_path.string(),
                  kMaxExtraFileBytes);
          continue;
        }
        std::filesystem::create_directories(extra_file_dir);
        std::filesystem::copy_file(src_path, dest);
        copied += size;
      } else {
        MW_WARN("Extra file path '{}' is neither regular file nor directory, skip copying.", src_path.string());
      }
    } catch (const std::filesystem::filesystem_error& e) {
      MW_WARN("Copy extra file path '{}' failed: {}", file_path_str, e.what());
    }
  }
}

void RecordAction::SetMcapOptions() {
  static std::unordered_map<std::string, mcap::Compression> compression_mode_map = {
      {"none", mcap::Compression::None},
      {"zstd", mcap::Compression::Zstd},
      {"lz4", mcap::Compression::Lz4},
  };
  static std::unordered_map<std::string, mcap::CompressionLevel> compression_level_map = {
      {"fastest", mcap::CompressionLevel::Fastest},
      {"fast", mcap::CompressionLevel::Fast},
      {"default", mcap::CompressionLevel::Default},
      {"slow", mcap::CompressionLevel::Slow},
      {"slowest", mcap::CompressionLevel::Slowest},
  };
  mcap_options.compression_mode = compression_mode_map[options_.storage_policy.compression_mode];
  mcap_options.compression_level = compression_level_map[options_.storage_policy.compression_level];
}

void RecordAction::BoundPreparationCache() {
  const auto bytes = [](const OneRecord& record) -> size_t {
    return record.buffer_view_ptr ? record.buffer_view_ptr->BufferSize() : 0;
  };
  cur_cache_bytes_ += bytes(cur_cache_.back());
  while (last_cache_bytes_ + cur_cache_bytes_ > max_preparation_cache_bytes_ &&
         last_cache_.size() + cur_cache_.size() > 1) {
    const bool from_last = !last_cache_.empty();
    auto& cache = from_last ? last_cache_ : cur_cache_;
    (from_last ? last_cache_bytes_ : cur_cache_bytes_) -= bytes(cache.front());
    cache.pop_front();
    if (dropped_cache_records_++ == 0) {
      MW_WARN("Records before the start exceed max_preparation_cache_m {} MB, dropping the oldest.",
              options_.max_preparation_cache_m);
    }
  }
}

bool RecordAction::HasEnoughFreeSpace() {
  if (options_.storage_policy.min_free_space_m == 0) return true;
  const uint64_t required = static_cast<uint64_t>(options_.storage_policy.min_free_space_m) * 1024 * 1024;
  uintmax_t available = 0;
  if (free_space_query_) {
    available = free_space_query_(parent_bag_path_);
  } else {
    std::error_code ec;
    available = std::filesystem::space(parent_bag_path_, ec).available;
    if (ec) {
      MW_WARN("Can not get free space of '{}': {}", parent_bag_path_.string(), ec.message());
      available = std::numeric_limits<uintmax_t>::max();
    }
  }
  if (available >= required) {
    if (write_paused_) MW_INFO("Free space of '{}' is back, recording resumes.", parent_bag_path_.string());
    write_paused_ = false;
    return true;
  }
  if (!write_paused_) {
    MW_ERROR("Free space of '{}' is {} MB, below min_free_space_m {} MB: recording pauses.",
             parent_bag_path_.string(), available / (1024 * 1024), options_.storage_policy.min_free_space_m);
  }
  write_paused_ = true;
  next_space_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  return false;
}

bool RecordAction::OpenNewFolderToRecord() {
  // Short of space, only the first folder is created (writing starts
  // paused): a new folder would stay empty and could push out folders with
  // data under max_folder_num.
  const bool has_space = HasEnoughFreeSpace();
  if (!has_space && !real_bag_path_.empty()) return false;

  auto tm = ibmw::common::util::GetCurTm();
  char buf[17];  // _YYYYMMDD_hhmmss
  snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
           (tm.tm_year + 1900) % 10000u, (tm.tm_mon + 1) % 100u, (tm.tm_mday) % 100u,
           (tm.tm_hour) % 100u, (tm.tm_min) % 100u, (tm.tm_sec) % 100u);
  auto new_bag_base_name = "ibmwbag" + std::string(buf);
  std::filesystem::path new_real_bag_path;

  // Names have second precision: a folder started in the same second gets a
  // "_01".."_99" suffix. create_directory() fails on an existing path, so a
  // folder is never shared.
  try {
    if (!parent_bag_path_.empty()) std::filesystem::create_directories(parent_bag_path_);
    for (uint32_t seq = 0; seq < kMaxBagFolderSeq && new_real_bag_path.empty(); ++seq) {
      auto candidate = parent_bag_path_ / new_bag_base_name;
      if (seq != 0) {
        char seq_buf[4];
        snprintf(seq_buf, sizeof(seq_buf), "_%02u", seq);
        candidate += seq_buf;
      }
      if (std::filesystem::create_directory(candidate)) new_real_bag_path = std::move(candidate);
    }
  } catch (const std::filesystem::filesystem_error& e) {
    MW_WARN("Create bag directory under '{}' failed: {}", parent_bag_path_.string(), e.what());
    return false;
  }
  if (new_real_bag_path.empty()) {
    MW_WARN("Bag path '{}' and its numbered variants already exist!", (parent_bag_path_ / new_bag_base_name).string());
    return false;
  }

  if (has_space && options_.storage_policy.max_folder_num > 0) {
    // Removing whole folders can take long: asynchronous once recording.
    if (state_.load() != State::kStart) {
      bag_retention::PruneBagFolders(parent_bag_path_, new_real_bag_path, options_.storage_policy.max_folder_num);
    } else {
      auto task_state = task_state_;
      executor_for_async_.Dispatch([task_state, parent = parent_bag_path_, keep = new_real_bag_path,
                                    max_num = options_.storage_policy.max_folder_num]() {
        RecordAction* action = nullptr;
        if (!task_state || !task_state->Enter(action)) {
          return;
        }
        struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
        bag_retention::PruneBagFolders(parent, keep, max_num);
      });
    }
  }

  real_bag_path_ = new_real_bag_path;
  metadata_yaml_file_path_ = real_bag_path_ / "metadata.yaml";
  metadata_.files.clear();
  cur_exec_count_ = 0;
  cur_data_size_ = 0;

  // use async operation to copy extra file path to new folder
  auto extra_file_dir = real_bag_path_ / "extra_file";
  if (state_.load() != State::kStart) {
    CopyExtraFilePathToNewFolder(extra_file_dir);
  } else {
    auto task_state = task_state_;
    executor_for_async_.Dispatch([task_state, extra_file_dir = std::move(extra_file_dir)]() {
      RecordAction* action = nullptr;
      if (!task_state || !task_state->Enter(action)) {
        return;
      }
      struct Guard { std::shared_ptr<TaskState> state; ~Guard() { state->Leave(); } } guard{task_state};
      action->CopyExtraFilePathToNewFolder(extra_file_dir);
    });
  }

  OpenNewMcapToRecord(ibmw::common::util::GetCurTimestampNs());

  return true;
}

void RecordAction::OpenNewMcapToRecord(uint64_t timestamp) {
  CloseRecord();
  if (!HasEnoughFreeSpace()) {
    writer_ = std::make_unique<mcap::McapWriter>();  // not opened: writes fail
    cur_mcap_file_path_.clear();
    return;
  }
  writer_ = std::make_unique<mcap::McapWriter>();

  char time_buf[16];
  time_t sec = static_cast<time_t>(timestamp / 1000000000ULL);
  auto tm = ibmw::common::util::TimeT2TmLocal(sec);
  snprintf(time_buf, sizeof(time_buf), "%04d%02d%02d_%02d%02d%02d",
           (tm.tm_year + 1900) % 10000u, (tm.tm_mon + 1) % 100u, (tm.tm_mday) % 100u,
           (tm.tm_hour) % 100u, (tm.tm_min) % 100u, (tm.tm_sec) % 100u);

  std::string cur_mcap_file_name = "ibmwbag_" + std::string(time_buf) + ".mcap";

  cur_mcap_file_path_ = (real_bag_path_ / cur_mcap_file_name).string();

  MW_INFO("Open new mcap file to record: {}", cur_mcap_file_path_);

  auto options = mcap::McapWriterOptions("ibmwbag");

  // setup compression mode and level
  options.compression = mcap_options.compression_mode;
  options.compressionLevel = mcap_options.compression_level;

  const auto res = writer_->open(cur_mcap_file_path_, options);
  if (!res.ok()) {
    MW_ERROR("Failed to open mcap file '{}': {}", cur_mcap_file_path_, res.message);
    cur_mcap_file_path_.clear();
    // Drop records and retry in a second, like a full disk.
    write_paused_ = true;
    next_space_check_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return;
  }
  write_paused_ = false;  // HasEnoughFreeSpace() leaves it set without min_free_space_m

  std::unordered_map<std::string, mcap::SchemaId> schema_id_by_identity;
  for (auto& [idx, mcap_info] : mcap_info_map_) {
    const std::string schema_identity =
        mcap_info.schema_name + '\0' + mcap_info.schema_format + '\0' +
        mcap_info.schema_hash;
    auto schema_id_itr = schema_id_by_identity.find(schema_identity);
    if (schema_id_itr == schema_id_by_identity.end()) {
      mcap::Schema schema(
          mcap_info.schema_name,
          mcap_info.schema_format,
          mcap_info.schema_data);
      writer_->addSchema(schema);
      schema_id_itr = schema_id_by_identity
                          .emplace(schema_identity, schema.id)
                          .first;
    }

    mcap::Channel channel(
        mcap_info.transport_name,
        mcap_info.transport_format,
        schema_id_itr->second);
    writer_->addChannel(channel);
    topic_runtime_map_[idx].transport_id = channel.id;
  }

  metadata_.files.emplace_back(
      MetaData::FileMeta{
          .path = cur_mcap_file_name,
          .start_timestamp = timestamp});
  // check and del record file
  if (options_.storage_policy.max_bag_num > 0 && metadata_.files.size() > options_.storage_policy.max_bag_num) {
    auto itr = metadata_.files.begin();
    std::filesystem::remove(real_bag_path_ / itr->path);
    metadata_.files.erase(itr);
  }
  if (options_.storage_policy.max_total_size_m > 0) {
    bag_retention::PruneBagFiles(real_bag_path_, metadata_.files,
                                 static_cast<uint64_t>(options_.storage_policy.max_total_size_m) * 1024 * 1024);
  }

  YAML::Node node;
  node["ibmw_bagfile_information"] = metadata_;

  WriteMetadataFile(metadata_yaml_file_path_, node);

  // write last msg in cache to mcap file
  writing_cached_messages_ = true;
  for (auto& [topic_id, topic_runtime_info] : topic_runtime_map_) {
    if (topic_runtime_info.cache_last_msg && topic_runtime_info.last_msg.buffer_view_ptr != nullptr) {
      MW_DEBUG("Write last msg in cache to mcap file: topic id {}, {} bytes.", topic_id,
               topic_runtime_info.last_msg.buffer_view_ptr->BufferSize());
      topic_runtime_info.last_msg.log_timestamp = timestamp;
      AddRecordImpl(std::move(topic_runtime_info.last_msg));
    }
  }
  writing_cached_messages_ = false;
}

namespace bag_retention {

namespace {

// "ibmwbag_YYYYMMDD_hhmmss" or "ibmwbag_YYYYMMDD_hhmmss_NN", as created by
// OpenNewFolderToRecord().
bool IsBagFolderName(const std::string& name) {
  constexpr std::string_view kPrefix = "ibmwbag_";
  constexpr size_t kBaseSize = kPrefix.size() + 15;
  if ((name.size() != kBaseSize && name.size() != kBaseSize + 3) ||
      name.compare(0, kPrefix.size(), kPrefix) != 0)
    return false;
  for (size_t i = kPrefix.size(); i < name.size(); ++i) {
    const bool sep = i == kPrefix.size() + 8 || i == kBaseSize;
    if (sep ? name[i] != '_' : (name[i] < '0' || name[i] > '9')) return false;
  }
  return true;
}

}  // namespace

void PruneBagFolders(const std::filesystem::path& parent, const std::filesystem::path& keep, uint32_t max_num) {
  if (max_num == 0) return;
  const auto keep_name = keep.filename().string();
  size_t folder_num = 1;  // `keep`
  std::vector<std::string> older;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
    auto name = it->path().filename().string();
    if (!IsBagFolderName(name) || name == keep_name) continue;
    // A symbolic link is not a folder this recorder created: never removed.
    std::error_code status_ec;
    if (!std::filesystem::is_directory(it->symlink_status(status_ec)) || status_ec) continue;
    ++folder_num;
    // A newer folder (another day started meanwhile) is never removed.
    if (name < keep_name) older.push_back(std::move(name));
  }
  if (ec) {
    MW_WARN("Can not list bag folders in '{}': {}", parent.string(), ec.message());
    return;
  }
  if (folder_num <= max_num) return;
  // The names sort by their timestamp.
  std::sort(older.begin(), older.end());
  const size_t remove_num = std::min(folder_num - max_num, older.size());
  for (size_t i = 0; i < remove_num; ++i) {
    const auto path = parent / older[i];
    std::filesystem::remove_all(path, ec);
    if (ec) {
      MW_WARN("Remove old bag folder '{}' failed: {}", path.string(), ec.message());
    } else {
      MW_INFO("Removed old bag folder '{}' (max_folder_num {}).", path.string(), max_num);
    }
  }
}

void PruneBagFiles(const std::filesystem::path& dir, std::deque<MetaData::FileMeta>& files, uint64_t max_bytes) {
  std::vector<uint64_t> sizes;
  sizes.reserve(files.size());
  uint64_t total = 0;
  for (const auto& file : files) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(dir / file.path, ec);
    sizes.push_back(ec ? 0 : size);
    total += sizes.back();
  }
  size_t removed = 0;
  while (total > max_bytes && files.size() - removed > 1) {
    const auto path = dir / files[removed].path;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) MW_WARN("Remove old bag file '{}' failed: {}", path.string(), ec.message());
    total -= sizes[removed];
    ++removed;
  }
  files.erase(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(removed));
}

}  // namespace bag_retention

}  // namespace ibmw::extensions::record_playback_extension
