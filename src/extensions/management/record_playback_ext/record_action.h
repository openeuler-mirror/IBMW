// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "yaml-cpp/yaml.h"

#include "cpp/executor/executor.h"
#include "cpp/executor/timer.h"
#include "cpp/util/buffer.h"
#include "cpp/util/schema_support.h"
#include "cpp/util/type_support.h"
#include "cpp/transport/topic_descriptor.h"
#include "mcap/writer.hpp"
#include "record_playback_ext/metadata_yaml.h"
#include "record_playback_ext/topic_meta.h"
#include "topic_meta.h"

namespace ibmw::extensions::record_playback_extension {

class RecordAction {
 public:
  struct Options {
    std::string bag_path;
    enum class Mode {
      kImd,
      kSignal,
    };
    Mode mode = Mode::kImd;

    struct StoragePolicy {
      uint32_t max_bag_size_m = 2048;
      uint32_t max_bag_num = 0;
      uint32_t msg_write_interval = 1000;
      uint32_t msg_write_interval_time = 1000;
      std::string compression_mode = "zstd";
      std::string compression_level = "default";
      bool new_folder_daily = false;
      // Disk usage limits, checked only when a bag file or folder is opened;
      // 0 disables a limit.
      // Writing pauses (messages are dropped) while the bag disk has less
      // free space than this; rechecked at most once a second. While paused
      // no new bag folder is created (a signal record fails to start).
      uint32_t min_free_space_m = 0;
      // Oldest ibmwbag_YYYYMMDD_hhmmss[_NN] folders under bag_path are removed so
      // that at most this many remain, the current one included. bag_path
      // must not be shared with another record action or writable by others.
      uint32_t max_folder_num = 0;
      // When a bag file is opened, the oldest files of the current folder are
      // removed while their total size exceeds this limit (the new file is
      // kept): the folder can grow up to this plus one max_bag_size_m.
      uint32_t max_total_size_m = 0;
    };

    StoragePolicy storage_policy;

    YAML::Node extra_attributes;

    uint64_t max_preparation_duration_s = 0;
    std::string executor;

    bool record_enabled = true;

    std::vector<TopicMeta> topic_meta_list;
    std::vector<std::string> extra_file_path;
  };

  struct OneRecord {
    uint64_t topic_index;
    uint64_t log_timestamp;
    uint64_t pub_timestamp;
    uint32_t pub_sequence;
    std::shared_ptr<ibmw::util::BufferArrayView> buffer_view_ptr;
  };

 public:
  RecordAction() = default;
  ~RecordAction() noexcept;

  RecordAction(const RecordAction&) = delete;
  RecordAction& operator=(const RecordAction&) = delete;

  void Initialize(YAML::Node options_node);
  void Start();
  void Shutdown();

  void InitExecutor(ibmw::executor::ExecutorHandle);

  const Options& GetOptions() const { return options_; }

  // Replaces the free space query of min_free_space_m (for tests). Must be
  // set before Initialize().
  void SetFreeSpaceQuery(std::function<uintmax_t(const std::filesystem::path&)> query) {
    free_space_query_ = std::move(query);
  }

  struct StatusSnapshot {
    bool record_enabled = true;
    std::vector<TopicMeta> topic_meta_list;
  };

  void GetStatusSnapshot(StatusSnapshot& snapshot, ibmw::util::DynamicLatch& latch);

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

  void RegisterGetTypeSupportFunc(
      const std::function<ibmw::util::TypeSupportRef(std::string_view)>& get_type_support_func);

  void RegisterGetSchemaSupportFunc(
      const std::function<std::optional<ibmw::util::SchemaDescriptor>(
          std::string_view, std::string_view)>& get_schema_support_func);

  const auto& GetTopicMetaMap() const { return topic_meta_map_; }
  void AddRecord(OneRecord&& record);

  bool StartSignalRecord(uint64_t preparation_duration_s, uint64_t record_duration_s, std::string& filefolder);
  bool StopSignalRecord();

  // New keys beyond this many extra attributes are dropped (each update
  // rewrites the whole metadata file).
  static constexpr size_t kMaxExtraAttributes = 1024;
  void UpdateMetadata(
      std::unordered_map<std::string, std::string>&& kv_pairs);

  enum class UpdateResult { kOk, kNotRunning, kTopicNotFound };
  // Applies all entries or none of them.
  UpdateResult UpdateTopicMetaRecord(std::vector<TopicMeta>&& topic_meta_list, std::optional<bool> action_record_enabled);

  // 0 disables sampling; otherwise the interval 1/freq must fit uint64 ns.
  static constexpr double kMinSampleFreq = 1e-9;
  static bool IsValidSampleFreq(double freq) {
    return freq == 0 || (std::isfinite(freq) && freq >= kMinSampleFreq);
  }
  static uint64_t SampleIntervalNs(double freq) {
    return freq > 0 ? static_cast<uint64_t>(1.0 / freq * 1000000000) : 0;
  }

  // Publisher timestamp/sequence metadata comes from the remote peer: an empty
  // value is 0, a malformed one is nullopt (never throws).
  static std::optional<uint64_t> ParsePubMeta(std::string_view value) {
    uint64_t result = 0;
    if (value.empty()) return result;
    const char* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(value.data(), end, result);
    if (ec != std::errc() || ptr != end) [[unlikely]] return std::nullopt;
    return result;
  }

 private:
  struct TaskState {
    std::mutex mutex;
    std::condition_variable cv;
    RecordAction* owner = nullptr;
    bool stopping = false;
    size_t active = 0;
    inline static thread_local TaskState* current_state = nullptr;
    inline static thread_local size_t current_depth = 0;

    bool Enter(RecordAction*& action) {
      std::lock_guard lock(mutex);
      if (stopping || owner == nullptr) {
        return false;
      }
      ++active;
      if (current_state == this) {
        ++current_depth;
      } else {
        current_state = this;
        current_depth = 1;
      }
      action = owner;
      return true;
    }

    void Leave() {
      std::lock_guard lock(mutex);
      MW_ASSERT(active > 0, "Record task accounting underflow.");
      --active;
      if (current_state == this) {
        MW_ASSERT(current_depth > 0, "Record task depth accounting underflow.");
        if (--current_depth == 0) current_state = nullptr;
      }
      cv.notify_all();
    }

    void StopAndWait() {
      std::unique_lock lock(mutex);
      stopping = true;
      owner = nullptr;
      const size_t own_depth = current_state == this ? current_depth : 0;
      cv.wait(lock, [this, own_depth] { return active <= own_depth; });
    }
  };

  void AddRecordImpl(OneRecord&& record);
  void OpenNewMcapToRecord(uint64_t start_timestamp);
  // Bag folders started in the same second: the base name, then _01.._99.
  static constexpr uint32_t kMaxBagFolderSeq = 100;
  bool OpenNewFolderToRecord();

  void CloseRecord();
  void FlushToDisk();
  void CopyExtraFilePathToNewFolder(const std::filesystem::path& extra_file_dir) const;

  void SetMcapOptions();

  size_t GetFileSize() const;
  bool IsNewFolderNeeded(uint64_t timestamp) const;
  bool HasEnoughFreeSpace();

  size_t GetDbFileSize() const;

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 private:
  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;
  ibmw::executor::ExecutorHandle executor_;

  struct McapStruct {
    std::string schema_name;
    std::string schema_format;
    std::string schema_data;
    std::string schema_hash;
    std::string transport_name;
    std::string transport_format;
  };

  struct TopicRuntimeInfo {
    uint16_t transport_id = 0;
    uint64_t last_timestamp = 0;
    uint64_t sample_interval = 0;
    bool record_enabled = true;
    bool cache_last_msg = true;
    OneRecord last_msg;
  };

  struct {
    mcap::Compression compression_mode;
    mcap::CompressionLevel compression_level;
  } mcap_options;

  std::string bag_base_name_;

  std::filesystem::path parent_bag_path_;
  std::filesystem::path real_bag_path_;

  std::unordered_map<uint64_t, McapStruct> mcap_info_map_;  // use to record

  std::unordered_map<uint64_t, TopicRuntimeInfo> topic_runtime_map_;

  std::string file_path_;
  std::string cur_mcap_file_path_;
  std::unique_ptr<mcap::McapWriter> writer_;
  size_t cur_data_size_;
  double estimated_overhead_ = 1.5;

  std::function<ibmw::util::TypeSupportRef(std::string_view)> get_type_support_func_;
  std::function<std::optional<ibmw::util::SchemaDescriptor>(
      std::string_view, std::string_view)> get_schema_support_func_;
  std::unordered_map<ibmw::transport::TopicMetaKey, TopicMeta,
                     ibmw::transport::TopicMetaKey::Hash>
      topic_meta_map_;

  std::shared_ptr<ibmw::executor::TimerBase> sync_timer_;
  ibmw::executor::ExecutorHandle executor_for_async_;  // use timer_executor for async operation

  size_t max_bag_size_ = 0;

  size_t cur_exec_count_ = 0;

  std::filesystem::path metadata_yaml_file_path_;
  MetaData metadata_;
  std::shared_ptr<TaskState> task_state_;

  // only for signal mode
  bool recording_flag_ = false;
  uint64_t max_preparation_duration_ns_ = 0;
  uint64_t stop_record_timestamp_ = 0;

  std::deque<OneRecord> last_cache_;
  std::deque<OneRecord> cur_cache_;

  // Indicates whether currently in the process of writing cached messages,
  // used to avoid triggering splitting and causing recursion during cache flush.
  bool writing_cached_messages_ = false;

  // Set while writing is paused for lack of free space (min_free_space_m).
  bool write_paused_ = false;
  std::chrono::steady_clock::time_point next_space_check_;
  std::function<uintmax_t(const std::filesystem::path&)> free_space_query_;
};

// Bag retention helpers (EXT-021), used when a bag folder or file is opened.
namespace bag_retention {

// Removes the oldest "ibmwbag_YYYYMMDD_hhmmss[_NN]" directories directly under
// `parent` so that at most `max_num` such directories remain, `keep`
// included. Never removes `keep` or a newer folder, symbolic links or other
// names.
void PruneBagFolders(const std::filesystem::path& parent, const std::filesystem::path& keep, uint32_t max_num);

// Removes the oldest of `files` (relative to `dir`), never the last one,
// while their total size exceeds `max_bytes`.
void PruneBagFiles(const std::filesystem::path& dir, std::deque<MetaData::FileMeta>& files, uint64_t max_bytes);

}  // namespace bag_retention

}  // namespace ibmw::extensions::record_playback_extension
