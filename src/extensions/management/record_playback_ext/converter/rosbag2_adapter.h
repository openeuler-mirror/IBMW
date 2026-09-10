// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "record_playback_ext/storage/bag_reader.h"
#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

struct BagConvertResult;

/**
 * @brief Reader adapter for a rosbag2 directory.
 *
 * rosbag2 exposes serialized messages, topic metadata and timestamps.  The
 * adapter translates those values to the common BagReader interface; payloads
 * are copied without deserialization or re-serialization.
 */
class Rosbag2BagReader final : public BagReader {
 public:
  void Open(
      const std::filesystem::path& bag_path,
      const ReaderOpenOptions& options = {}) override;
  void ReadMessages(
      const BagReadOptions& options,
      const MessageCallback& callback) const override;
  const BagInfo& Info() const noexcept override { return info_; }

 private:
  BagInfo info_;
  std::filesystem::path bag_path_;
  std::string storage_id_;
  std::filesystem::path storage_config_file_;
};

/**
 * @brief Convert a common BagReader stream to a rosbag2 directory.
 *
 * This is deliberately separate from the IBMW Storage interface because a
 * rosbag2 writer owns its metadata.yaml and uses TopicMetadata rather than
 * IBMW Schema/Channel tables.
 */
BagConvertResult ConvertBagToRosbag2(
    const BagReader& reader,
    const StorageOptions& options);

}  // namespace ibmw::extensions::record_playback_extension
