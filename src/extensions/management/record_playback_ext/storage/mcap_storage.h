// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "mcap/writer.hpp"
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

  static std::string SchemaKey(
      const ibmw::util::SchemaDescriptor& schema);
  static std::string ChannelKey(
      const StorageChannelDescriptor& channel);
  static std::string MessageEncoding(std::string_view serialization_type);

  void WriteMetadata();

  State state_ = State::kCreated;
  StorageOptions options_;
  StorageSummary summary_;
  std::filesystem::path mcap_path_;
  std::unique_ptr<mcap::McapWriter> writer_;
  bool requested_clean_close_ = true;
  std::unordered_map<std::string, StoredSchema> schemas_by_key_;
  std::unordered_map<StorageSchemaId, ibmw::util::SchemaDescriptor>
      schemas_by_id_;
  std::unordered_map<std::string, StoredChannel> channels_by_key_;
  std::unordered_map<StorageChannelId, StoredChannel> channels_by_id_;
};

}  // namespace ibmw::extensions::record_playback_extension
