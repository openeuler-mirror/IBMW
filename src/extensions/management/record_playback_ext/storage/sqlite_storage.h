// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

#include <sqlite3.h>

#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

class SqliteStorage final : public Storage {
 public:
  SqliteStorage() = default;
  ~SqliteStorage() override;

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

  bool IsOpen() const noexcept override { return open_; }
  const StorageSummary& Summary() const noexcept override { return summary_; }

 private:
  struct StoredChannel {
    StorageSchemaId schema_id = 0;
    StorageChannelDescriptor descriptor;
  };

  void OpenNewFile();
  void CloseCurrentFile();
  void CreateSchema();
  void RestoreDescriptors();
  void ApplyConfiguration();
  bool ShouldSplit(uint64_t log_time, size_t payload_size) const;
  void Execute(std::string_view sql) const;
  void WriteMetadata();
  static std::string SchemaKey(const ibmw::util::SchemaDescriptor& schema);
  static std::string ChannelKey(const StorageChannelDescriptor& channel);

  bool open_ = false;
  StorageOptions options_;
  StorageSummary summary_;
  sqlite3* database_ = nullptr;
  sqlite3_stmt* insert_message_ = nullptr;
  std::filesystem::path database_path_;
  size_t file_index_ = 0;
  uint64_t current_file_start_timestamp_ = 0;
  uint64_t current_file_end_timestamp_ = 0;
  uint64_t current_file_message_count_ = 0;
  uint64_t current_file_estimated_size_ = 0;
  bool requested_clean_close_ = true;
  std::string current_split_reason_;
  StorageSchemaId next_schema_id_ = 1;
  StorageChannelId next_channel_id_ = 1;
  std::unordered_map<std::string, StorageSchemaId> schema_ids_;
  std::unordered_map<StorageSchemaId, ibmw::util::SchemaDescriptor> schemas_;
  std::unordered_map<std::string, StorageChannelId> channel_ids_;
  std::unordered_map<StorageChannelId, StoredChannel> channels_;
};

}  // namespace ibmw::extensions::record_playback_extension
