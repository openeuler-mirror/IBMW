// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/transport/serialized_message.h"

namespace ibmw::extensions::record_playback_extension {

struct BagFileInfo {
  std::filesystem::path path;
  uint64_t start_timestamp = 0;
  uint64_t end_timestamp = 0;
  uint64_t message_count = 0;
  uint64_t size = 0;
  std::string compression = "none";
  std::string split_reason;
};

struct BagSchemaInfo {
  uint16_t id = 0;
  ibmw::util::SchemaDescriptor descriptor;
  bool hash_valid = false;
};

struct BagChannelInfo {
  uint16_t id = 0;
  uint16_t schema_id = 0;
  ibmw::runtime::core::transport::TopicDescriptor topic;
  std::string source_id;
  std::string publisher_id;
  std::string record_driver;
  uint64_t message_count = 0;
};

struct BagInfo {
  std::filesystem::path bag_path;
  uint32_t version = 0;
  std::string storage_id;
  std::vector<BagFileInfo> files;
  std::vector<BagSchemaInfo> schemas;
  std::vector<BagChannelInfo> channels;
  uint64_t start_timestamp = 0;
  uint64_t end_timestamp = 0;
  uint64_t message_count = 0;
  uint64_t total_size = 0;
  uint64_t dropped_message_count = 0;
  uint64_t duplicate_message_count = 0;
  std::string error_message;
  bool closed_cleanly = false;

  uint64_t Duration() const noexcept {
    return message_count == 0 ? 0 : end_timestamp - start_timestamp;
  }
};

struct BagReadOptions {
  uint64_t start_timestamp = 0;
  uint64_t end_timestamp = 0;
  std::vector<std::string> topics;
};

struct BagMessage {
  BagChannelInfo channel;
  uint64_t log_time = 0;
  uint64_t publish_time = 0;
  uint32_t sequence = 0;
  std::shared_ptr<ibmw::util::BufferArrayView> payload;
};

class McapBagReader {
 public:
  using MessageCallback = std::function<bool(BagMessage&& message)>;

  void Open(
      const std::filesystem::path& bag_path,
      std::string_view expected_storage_id = {});
  void ReadMessages(
      const BagReadOptions& options,
      const MessageCallback& callback) const;

  const BagInfo& Info() const noexcept { return info_; }

 private:
  struct FileChannelKey {
    size_t file_index = 0;
    uint16_t channel_id = 0;

    bool operator==(const FileChannelKey&) const = default;
  };

  struct FileChannelKeyHash {
    size_t operator()(const FileChannelKey& key) const noexcept {
      return (key.file_index << 16) ^ key.channel_id;
    }
  };

  BagInfo info_;
  std::unordered_map<FileChannelKey, BagChannelInfo, FileChannelKeyHash>
      file_channels_;
};

}  // namespace ibmw::extensions::record_playback_extension
