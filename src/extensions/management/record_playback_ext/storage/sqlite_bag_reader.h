// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <utility>
#include <unordered_map>
#include <vector>

#include "record_playback_ext/storage/mcap_bag_reader.h"

namespace ibmw::extensions::record_playback_extension {

class SqliteBagReader final : public BagReader {
 public:
  void Open(
      const std::filesystem::path& bag_path,
      const ReaderOpenOptions& options = {}) override;
  void ReadMessages(
      const BagReadOptions& options,
      const MessageCallback& callback) const override;
  const BagInfo& Info() const noexcept override { return info_; }

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
  int busy_timeout_ms_ = 0;
  std::vector<std::pair<std::string, int64_t>> pragmas_;
  std::unordered_map<FileChannelKey, BagChannelInfo, FileChannelKeyHash>
      file_channels_;
};

}  // namespace ibmw::extensions::record_playback_extension
