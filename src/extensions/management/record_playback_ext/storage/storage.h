// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/util/schema_support.h"
#include "cpp/transport/serialized_message.h"

namespace ibmw::extensions::record_playback_extension {

using StorageSchemaId = uint16_t;
using StorageChannelId = uint16_t;

struct StorageOptions {
  std::filesystem::path output_path;
  std::string storage_id = "mcap";
  uint64_t max_bag_size = 0;
  uint64_t max_bag_duration_ns = 0;
  std::string compression_mode = "none";
  std::string compression_format;
  size_t compression_queue_size = 1;
  size_t compression_threads = 1;
  std::string storage_preset_profile;
  std::filesystem::path storage_config_file;
};

struct StorageFileSummary {
  std::filesystem::path relative_path;
  uint64_t start_timestamp = 0;
  uint64_t end_timestamp = 0;
  uint64_t message_count = 0;
  uint64_t file_size = 0;
  std::string compression = "none";
  std::string split_reason;
};

struct StorageChannelDescriptor {
  ibmw::transport::TopicDescriptor topic;
};

struct StorageSummary {
  std::filesystem::path output_path;
  std::vector<std::filesystem::path> relative_files;
  uint64_t start_timestamp = 0;
  uint64_t end_timestamp = 0;
  uint64_t message_count = 0;
  uint64_t file_size = 0;
  uint64_t dropped_message_count = 0;
  uint64_t duplicate_message_count = 0;
  std::string error_message;
  std::unordered_map<StorageChannelId, uint64_t> channel_message_counts;
  std::vector<StorageFileSummary> files;
  bool closed_cleanly = false;
};

class Storage {
 public:
  Storage() = default;
  virtual ~Storage() = default;

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  virtual void Open(const StorageOptions& options) = 0;
  virtual StorageSchemaId RegisterSchema(
      const ibmw::util::SchemaDescriptor& schema) = 0;
  virtual StorageChannelId RegisterChannel(
      const StorageChannelDescriptor& channel,
      StorageSchemaId schema_id) = 0;
  virtual void Write(
      StorageChannelId channel_id,
      const ibmw::transport::SerializedMessage& message) = 0;
  virtual void Flush() = 0;
  virtual void SetMessageStatistics(
      uint64_t dropped_message_count,
      uint64_t duplicate_message_count) = 0;
  virtual void SetCloseStatus(
      bool clean,
      std::string error_message) {
    static_cast<void>(clean);
    static_cast<void>(error_message);
  }
  virtual void Close() = 0;

  virtual bool IsOpen() const noexcept = 0;
  virtual const StorageSummary& Summary() const noexcept = 0;
};

}  // namespace ibmw::extensions::record_playback_extension
