// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
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
      ibmw::runtime::core::transport::SerializedMessage message);
  void Close();

  const StorageSummary& Summary() const noexcept { return storage_->Summary(); }
  RecordQueue::Statistics QueueStatistics() const {
    return queue_.GetStatistics();
  }

 private:
  enum class State {
    kCreated,
    kInitialized,
    kRunning,
    kClosed,
  };

  void RunWriter();
  StorageSchemaId FindOrRegisterSchema(
      const ibmw::runtime::core::transport::TopicDescriptor& topic);
  StorageChannelId FindOrRegisterChannel(
      const ibmw::runtime::core::transport::SerializedMessage& message,
      StorageSchemaId schema_id);
  static std::string SchemaKey(
      const ibmw::runtime::core::transport::TopicDescriptor& topic);
  static std::string ChannelKey(
      const ibmw::runtime::core::transport::TopicDescriptor& topic);

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
};

}  // namespace ibmw::extensions::record_playback_extension
