// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "record_playback_ext/recorder/record_queue.h"
#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

class RecorderCoordinator {
 public:
  struct Options {
    StorageOptions storage;
    RecordQueue::Options queue;
    bool snapshot_mode = false;
  };

  explicit RecorderCoordinator(
      Options options,
      std::unique_ptr<Storage> storage);
  ~RecorderCoordinator();

  RecorderCoordinator(const RecorderCoordinator&) = delete;
  RecorderCoordinator& operator=(const RecorderCoordinator&) = delete;

  void Initialize();
  void Start();
  RecordQueue::PushResult Accept(
      ibmw::transport::SerializedMessage message);
  // False when the channel's Schema conflicts with the one already recorded
  // for its type and serialization; the channel is not registered.
  bool RegisterChannel(StorageChannelDescriptor channel);
  void TriggerSnapshot();
  void AddDroppedMessages(uint64_t count = 1) {
    external_dropped_.fetch_add(count);
  }
  void Close();
  // Messages dropped because their Schema conflicts with the one already
  // recorded for the same type and serialization. Counted as dropped.
  uint64_t SchemaConflictMessages() const noexcept {
    return schema_conflict_messages_.load();
  }

  const StorageSummary& Summary() const noexcept { return storage_->Summary(); }
  RecordQueue::Statistics QueueStatistics() const;

 private:
  enum class State {
    kCreated,
    kInitialized,
    kRunning,
    kClosed,
  };

  void RunWriter();
  void WriteMessage(
      const ibmw::transport::SerializedMessage& message);
  void BufferSnapshot(
      ibmw::transport::SerializedMessage message,
      uint64_t queue_sequence);
  void FlushSnapshot(uint64_t queue_target);
  void FlushReadySnapshots(uint64_t popped_messages);
  std::optional<StorageSchemaId> FindOrRegisterSchema(
      const ibmw::transport::TopicDescriptor& topic);
  StorageChannelId FindOrRegisterChannel(
      const ibmw::transport::SerializedMessage& message,
      StorageSchemaId schema_id);
  StorageChannelId FindOrRegisterChannel(
      const StorageChannelDescriptor& channel,
      StorageSchemaId schema_id);
  static std::string SchemaKey(
      const ibmw::transport::TopicDescriptor& topic);
  static std::string ChannelKey(
      const ibmw::transport::TopicDescriptor& topic);

  const Options options_;
  std::unique_ptr<Storage> storage_;
  RecordQueue queue_;
  std::atomic<State> state_ = State::kCreated;
  std::thread writer_thread_;
  std::unordered_map<std::string, StorageSchemaId> schema_ids_;
  std::unordered_map<std::string, ibmw::util::SchemaDescriptor>
      schemas_by_key_;
  std::unordered_map<std::string, StorageChannelId> channel_ids_;
  mutable std::mutex error_mutex_;
  std::exception_ptr writer_error_;
  std::atomic<uint64_t> writer_failed_messages_{0};
  std::atomic<uint64_t> schema_conflict_messages_{0};
  struct SnapshotBufferedMessage {
    uint64_t queue_sequence;
    ibmw::transport::SerializedMessage message;
  };
  std::deque<SnapshotBufferedMessage> snapshot_buffer_;
  size_t snapshot_buffer_bytes_ = 0;
  uint64_t snapshot_dropped_ = 0;
  struct SnapshotRequest {
    uint64_t id;
    uint64_t queue_target;
  };
  std::mutex snapshot_input_mutex_;
  std::mutex snapshot_mutex_;
  std::condition_variable snapshot_cv_;
  std::deque<SnapshotRequest> snapshot_requests_;
  uint64_t next_snapshot_id_ = 1;
  uint64_t completed_snapshot_id_ = 0;
  bool snapshot_writer_finished_ = false;
  std::atomic<uint64_t> external_dropped_{0};
  std::mutex storage_mutex_;
};

}  // namespace ibmw::extensions::record_playback_extension
