// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "cpp/executor/executor.h"
#include "cpp/util/buffer.h"
#include "cpp/util/type_support.h"
#include "record_playback_ext/metadata_yaml.h"
#include "record_playback_ext/topic_meta.h"

#include "mcap/reader.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

class PlaybackAction {
 public:
  struct Options {
    std::string bag_path;

    enum class Mode {
      kImd,
      kSignal,
    };
    Mode mode = Mode::kImd;

    std::string executor;

    uint64_t skip_duration_s = 0;
    uint64_t play_duration_s = 0;

    struct TopicMeta {
      std::string topic_name;
      std::string msg_type;
    };
    std::vector<TopicMeta> topic_meta_list;
  };

  struct OneRecord {
    uint64_t topic_index;
    uint64_t dt;
    std::shared_ptr<ibmw::util::BufferArrayView> buffer_view_ptr;
  };

 public:
  PlaybackAction() = default;
  ~PlaybackAction() noexcept;

  PlaybackAction(const PlaybackAction&) = delete;
  PlaybackAction& operator=(const PlaybackAction&) = delete;

  void Initialize(YAML::Node options_node);
  void Start();
  void Shutdown();

  void InitExecutor();

  const Options& GetOptions() const { return options_; }

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

  void RegisterGetTypeSupportFunc(
      const std::function<ibmw::util::TypeSupportRef(std::string_view)>& get_type_support_func);

  const auto& GetTopicMetaMap() const { return topic_meta_map_; }
  void RegisterPubRecordFunc(std::function<void(const OneRecord&)>&& func);

  bool StartSignalPlayback(uint64_t skip_duration_s, uint64_t play_duration_s);
  void StopSignalPlayback();

 private:
  struct TaskState {
    std::mutex mutex;
    std::condition_variable cv;
    PlaybackAction* owner = nullptr;
    bool stopping = false;
    size_t active = 0;
    inline static thread_local TaskState* current_state = nullptr;
    inline static thread_local size_t current_depth = 0;

    bool Enter(PlaybackAction*& action) {
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
      MW_ASSERT(active > 0, "Playback task accounting underflow.");
      --active;
      if (current_state == this) {
        MW_ASSERT(current_depth > 0, "Playback task depth accounting underflow.");
        --current_depth;
        if (current_depth == 0) {
          current_state = nullptr;
        }
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

  void StartPlaybackImpl(uint64_t skip_duration_s, uint64_t play_duration_s);

  void AddPlaybackTasks(const std::shared_ptr<TaskState>& task_state);

  bool OpenNewMcaptoPlayBack();

  void ClosePlayback();

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

  std::function<ibmw::util::TypeSupportRef(std::string_view)> get_type_support_func_;
  std::unordered_map<uint64_t, TopicMeta> topic_meta_map_;
  std::unordered_map<std::string, uint64_t> topic_name_to_topic_id_map_;
  std::unordered_map<uint64_t, uint64_t> channel_id_to_topic_id_map_;

  std::unique_ptr<mcap::McapReader> reader_;
  std::unique_ptr<mcap::LinearMessageView> msg_reader_ptr_;
  std::unique_ptr<mcap::LinearMessageView::Iterator> msg_reader_itr_;

  MetaData metadata_;

  std::function<void(const OneRecord&)> pub_record_func_;

  std::filesystem::path real_bag_path_;

  size_t cur_mcap_file_index_ = 0;
  uint64_t start_timestamp_ = 0;

  uint64_t start_playback_timestamp_ = 0;
  uint64_t stop_playback_timestamp_ = 0;

  std::string select_msg_sql_topic_id_range_;
  std::mutex db_mutex_;
  std::shared_ptr<TaskState> task_state_;

  enum class PlayBackState {
    kReadyToPlay,
    kPlaying,
    kGetStopSignal,
  };
  PlayBackState playback_state_ = PlayBackState::kReadyToPlay;
  std::mutex playback_state_mutex_;
};

}  // namespace ibmw::extensions::record_playback_extension
