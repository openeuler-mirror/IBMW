// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <deque>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>

#include "mcap/writer.hpp"
#include "record_playback_ext/storage/compression_task_pool.h"
#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

class McapStorage final : public Storage {
 public:
  McapStorage() = default;
  ~McapStorage() override;

  void Open(const StorageOptions& options) override;
  StorageSchemaId RegisterSchema(
      const ibmw::util::SchemaDescriptor& schema) override;
  StorageChannelId RegisterChannel(
      const StorageChannelDescriptor& channel,
      StorageSchemaId schema_id) override;
  void Write(
      StorageChannelId channel_id,
      const ibmw::transport::SerializedMessage& message)
      override;
  void Flush() override;
  void SetMessageStatistics(
      uint64_t dropped_message_count,
      uint64_t duplicate_message_count) override;
  void SetCloseStatus(
      bool clean,
      std::string error_message) override;
  void Close() override;

  bool IsOpen() const noexcept override { return state_ == State::kOpen; }
  const StorageSummary& Summary() const noexcept override { return summary_; }

 private:
  enum class State {
    kCreated,
    kOpen,
    kClosed,
  };

  struct StoredSchema {
    StorageSchemaId id;
    ibmw::util::SchemaDescriptor descriptor;
  };

  struct StoredChannel {
    StorageChannelId id;
    StorageSchemaId schema_id;
    StorageChannelDescriptor descriptor;
  };

  struct PendingMessage {
    mcap::ChannelId channel_id;
    uint32_t sequence;
    uint64_t log_time;
    uint64_t publish_time;
    std::future<std::string> payload;
  };

  struct PendingFile {
    StorageFileSummary summary;
    std::future<std::string> output_path;
  };

  static std::string SchemaKey(
      const ibmw::util::SchemaDescriptor& schema);
  static std::string ChannelKey(
      const StorageChannelDescriptor& channel);
  static std::string MessageEncoding(std::string_view serialization_type);

  void OpenNewFile(std::string split_reason = {});
  void CloseCurrentFile();
  void WriteMcapMessage(
      mcap::ChannelId channel_id,
      uint32_t sequence,
      uint64_t log_time,
      uint64_t publish_time,
      const std::string& payload);
  void AccountMessage(
      StorageChannelId channel_id,
      uint64_t log_time,
      size_t payload_size);
  void DrainPendingMessage();
  void DrainPendingMessages();
  void DrainPendingFile();
  void DrainPendingFiles();
  bool ShouldSplit(uint64_t log_time, size_t payload_size) const;
  mcap::Compression Compression() const;
  void WriteMetadata();

  State state_ = State::kCreated;
  StorageOptions options_;
  StorageSummary summary_;
  std::filesystem::path mcap_path_;
  std::unique_ptr<mcap::McapWriter> writer_;
  std::unique_ptr<CompressionTaskPool> compression_pool_;
  std::deque<PendingMessage> pending_messages_;
  std::deque<PendingFile> pending_files_;
  int compression_level_ = 0;
  size_t file_index_ = 0;
  uint64_t current_file_start_timestamp_ = 0;
  uint64_t current_file_end_timestamp_ = 0;
  uint64_t current_file_message_count_ = 0;
  uint64_t current_file_estimated_size_ = 0;
  std::string current_split_reason_;
  bool requested_clean_close_ = true;
  StorageSchemaId next_schema_id_ = 1;
  StorageChannelId next_channel_id_ = 1;
  std::unordered_map<StorageSchemaId, mcap::SchemaId> current_schema_ids_;
  std::unordered_map<StorageChannelId, mcap::ChannelId> current_channel_ids_;
  std::unordered_map<std::string, StoredSchema> schemas_by_key_;
  std::unordered_map<StorageSchemaId, ibmw::util::SchemaDescriptor>
      schemas_by_id_;
  std::unordered_map<std::string, StoredChannel> channels_by_key_;
  std::unordered_map<StorageChannelId, StoredChannel> channels_by_id_;
};

}  // namespace ibmw::extensions::record_playback_extension
