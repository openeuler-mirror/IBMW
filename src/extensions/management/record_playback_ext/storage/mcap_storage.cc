// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/mcap_storage.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string_view>

#include "base/exception.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr uint32_t kBagVersion = 1;

void ValidateSchema(const ibmw::util::SchemaDescriptor& schema) {
  MW_ASSERT(!schema.type_name.empty(), "Schema type name cannot be empty.");
  MW_ASSERT(
      !schema.serialization_type.empty(),
      "Schema serialization type cannot be empty for '{}'.",
      schema.type_name);
  MW_ASSERT(
      !schema.schema_name.empty() && !schema.schema_encoding.empty() &&
          !schema.schema_data.empty() && !schema.schema_hash.empty(),
      "Schema '{}' contains an empty required field.", schema.type_name);
  MW_ASSERT(
      schema.schema_hash == ibmw::util::ComputeSchemaHash(schema.schema_data),
      "Schema hash mismatch for '{}'.", schema.type_name);
}

}  // namespace

McapStorage::~McapStorage() {
  try {
    Close();
  } catch (...) {
  }
}

void McapStorage::Open(const StorageOptions& options) {
  MW_ASSERT(state_ == State::kCreated, "Storage can only be opened once.");
  MW_ASSERT(!options.output_path.empty(), "Bag output path cannot be empty.");
  MW_ASSERT(
      options.storage_id == "mcap",
      "McapStorage cannot open storage id '{}'.", options.storage_id);

  options_ = options;
  options_.output_path = std::filesystem::absolute(options.output_path);
  if (std::filesystem::exists(options_.output_path)) {
    MW_ASSERT(
        std::filesystem::is_directory(options_.output_path),
        "Bag output '{}' is not a directory.", options_.output_path.string());
    MW_ASSERT(
        std::filesystem::is_empty(options_.output_path),
        "Bag output directory '{}' is not empty.",
        options_.output_path.string());
  } else {
    std::filesystem::create_directories(options_.output_path);
  }

  mcap_path_ = options_.output_path / "data_0.mcap";
  writer_ = std::make_unique<mcap::McapWriter>();
  auto writer_options = mcap::McapWriterOptions("ibmw");
  writer_options.compression = mcap::Compression::None;
  const auto result = writer_->open(mcap_path_.string(), writer_options);
  MW_ASSERT(
      result.ok(), "Failed to open MCAP '{}': {}.", mcap_path_.string(),
      result.message);

  summary_.output_path = options_.output_path;
  summary_.relative_files = {mcap_path_.filename()};
  state_ = State::kOpen;
}

StorageSchemaId McapStorage::RegisterSchema(
    const ibmw::util::SchemaDescriptor& schema) {
  MW_ASSERT(state_ == State::kOpen, "Storage is not open.");
  ValidateSchema(schema);

  const std::string key = SchemaKey(schema);
  const auto existing = schemas_by_key_.find(key);
  if (existing != schemas_by_key_.end()) {
    MW_ASSERT(
        existing->second.descriptor.schema_hash == schema.schema_hash &&
            existing->second.descriptor.schema_name == schema.schema_name &&
            existing->second.descriptor.schema_encoding ==
                schema.schema_encoding,
        "Conflicting Schema for type '{}' serialization '{}'.",
        schema.type_name, schema.serialization_type);
    return existing->second.id;
  }

  mcap::Schema mcap_schema(
      schema.schema_name, schema.schema_encoding, schema.schema_data);
  writer_->addSchema(mcap_schema);
  MW_ASSERT(mcap_schema.id != 0, "MCAP returned an invalid Schema ID.");

  const auto id = static_cast<StorageSchemaId>(mcap_schema.id);
  schemas_by_key_.emplace(
      key, StoredSchema{.id = id, .descriptor = schema});
  schemas_by_id_.emplace(id, schema);
  return id;
}

StorageChannelId McapStorage::RegisterChannel(
    const StorageChannelDescriptor& channel,
    StorageSchemaId schema_id) {
  MW_ASSERT(state_ == State::kOpen, "Storage is not open.");
  const auto schema = schemas_by_id_.find(schema_id);
  MW_ASSERT(schema != schemas_by_id_.end(), "Schema ID {} is not registered.", schema_id);
  MW_ASSERT(!channel.topic.topic.empty(), "Channel topic cannot be empty.");
  MW_ASSERT(
      channel.topic.msg_type == schema->second.type_name,
      "Channel type '{}' does not match Schema type '{}'.",
      channel.topic.msg_type, schema->second.type_name);
  MW_ASSERT(
      channel.topic.serialization_type == schema->second.serialization_type,
      "Channel serialization '{}' does not match Schema serialization '{}'.",
      channel.topic.serialization_type, schema->second.serialization_type);
  const std::string key = ChannelKey(channel);
  const auto existing = channels_by_key_.find(key);
  if (existing != channels_by_key_.end()) {
    MW_ASSERT(
        existing->second.schema_id == schema_id,
        "Existing Channel '{}' references a different Schema.",
        channel.topic.topic);
    return existing->second.id;
  }

  mcap::KeyValueMap metadata{
      {"ibmw_msg_type", channel.topic.msg_type},
      {"serialization_type", channel.topic.serialization_type},
      {"schema_hash", schema->second.schema_hash}};
  mcap::Channel mcap_channel(
      channel.topic.topic,
      MessageEncoding(channel.topic.serialization_type),
      static_cast<mcap::SchemaId>(schema_id), metadata);
  writer_->addChannel(mcap_channel);
  MW_ASSERT(mcap_channel.id != 0, "MCAP returned an invalid Channel ID.");

  const auto id = static_cast<StorageChannelId>(mcap_channel.id);
  StoredChannel stored{
      .id = id, .schema_id = schema_id, .descriptor = channel};
  channels_by_key_.emplace(key, stored);
  channels_by_id_.emplace(id, std::move(stored));
  summary_.channel_message_counts.emplace(id, 0);
  return id;
}

void McapStorage::Write(
    StorageChannelId channel_id,
    const ibmw::runtime::core::transport::SerializedMessage& message) {
  MW_ASSERT(state_ == State::kOpen, "Storage is not open.");
  const auto channel = channels_by_id_.find(channel_id);
  MW_ASSERT(channel != channels_by_id_.end(), "Channel ID {} is not registered.", channel_id);
  MW_ASSERT(message.payload != nullptr, "Cannot write a null payload.");
  MW_ASSERT(
      message.topic.topic == channel->second.descriptor.topic.topic &&
          message.topic.msg_type == channel->second.descriptor.topic.msg_type &&
          message.topic.serialization_type ==
              channel->second.descriptor.topic.serialization_type,
      "Message descriptor does not match Channel ID {}.", channel_id);

  const uint64_t log_time =
      message.receive_timestamp != 0 ? message.receive_timestamp
                                     : message.publish_timestamp;
  const uint64_t publish_time =
      message.publish_timestamp != 0 ? message.publish_timestamp : log_time;
  MW_ASSERT(log_time != 0, "Message timestamp cannot be zero.");

  const std::string payload = message.payload->JoinToString();
  mcap::Message mcap_message{
      .channelId = static_cast<mcap::ChannelId>(channel_id),
      .sequence = static_cast<uint32_t>(
          message.identity.sequence & std::numeric_limits<uint32_t>::max()),
      .logTime = log_time,
      .publishTime = publish_time,
      .dataSize = static_cast<uint64_t>(payload.size()),
      .data = reinterpret_cast<const std::byte*>(payload.data())};
  const auto result = writer_->write(mcap_message);
  MW_ASSERT(result.ok(), "Failed to write MCAP message: {}.", result.message);

  if (summary_.message_count == 0) {
    summary_.start_timestamp = log_time;
    summary_.end_timestamp = log_time;
  } else {
    summary_.start_timestamp = std::min(summary_.start_timestamp, log_time);
    summary_.end_timestamp = std::max(summary_.end_timestamp, log_time);
  }
  ++summary_.message_count;
  ++summary_.channel_message_counts[channel_id];
}

void McapStorage::Close() {
  if (state_ != State::kOpen) {
    return;
  }

  writer_->close();
  writer_.reset();
  summary_.file_size = std::filesystem::file_size(mcap_path_);
  summary_.closed_cleanly = requested_clean_close_;
  WriteMetadata();
  state_ = State::kClosed;
}

void McapStorage::SetCloseStatus(
    bool clean,
    std::string error_message) {
  requested_clean_close_ = clean;
  summary_.error_message = std::move(error_message);
}
std::string McapStorage::SchemaKey(
    const ibmw::util::SchemaDescriptor& schema) {
  return schema.type_name + '\0' + schema.serialization_type;
}

std::string McapStorage::ChannelKey(
    const StorageChannelDescriptor& channel) {
  return channel.topic.topic + '\0' + channel.topic.msg_type + '\0' +
         channel.topic.serialization_type + '\0' +
         channel.topic.schema.schema_hash;
}

std::string McapStorage::MessageEncoding(
    std::string_view serialization_type) {
  return serialization_type == "ros2" ? "cdr"
                                       : std::string(serialization_type);
}

void McapStorage::WriteMetadata() {
  YAML::Node information;
  information["version"] = kBagVersion;
  information["storage_id"] = options_.storage_id;
  information["message_count"] = summary_.message_count;
  information["start_timestamp"] = summary_.start_timestamp;
  information["end_timestamp"] = summary_.end_timestamp;
  information["duration_ns"] =
      summary_.message_count == 0
          ? 0
          : summary_.end_timestamp - summary_.start_timestamp;
  information["closed_cleanly"] = summary_.closed_cleanly;
  information["error_message"] = summary_.error_message;

  YAML::Node files(YAML::NodeType::Sequence);
  YAML::Node file;
  file["path"] = mcap_path_.filename().string();
  file["start_timestamp"] = summary_.start_timestamp;
  file["size"] = summary_.file_size;
  files.push_back(file);
  information["files"] = files;

  YAML::Node schemas(YAML::NodeType::Sequence);
  for (const auto& [id, schema] : schemas_by_id_) {
    YAML::Node item;
    item["id"] = id;
    item["type"] = schema.type_name;
    item["serialization_type"] = schema.serialization_type;
    item["name"] = schema.schema_name;
    item["encoding"] = schema.schema_encoding;
    item["hash"] = schema.schema_hash;
    item["data_size"] = schema.schema_data.size();
    schemas.push_back(item);
  }
  information["schemas"] = schemas;

  YAML::Node topics(YAML::NodeType::Sequence);
  YAML::Node channels(YAML::NodeType::Sequence);
  for (const auto& [id, channel] : channels_by_id_) {
    YAML::Node topic;
    topic["id"] = id;
    topic["topic"] = channel.descriptor.topic.topic;
    topic["msg_type"] = channel.descriptor.topic.msg_type;
    topic["serialization_type"] =
        channel.descriptor.topic.serialization_type;
    topics.push_back(topic);

    YAML::Node item;
    item["id"] = id;
    item["schema_id"] = channel.schema_id;
    item["topic"] = channel.descriptor.topic.topic;
    item["msg_type"] = channel.descriptor.topic.msg_type;
    item["serialization_type"] =
        channel.descriptor.topic.serialization_type;
    item["message_count"] = summary_.channel_message_counts.at(id);
    channels.push_back(item);
  }
  information["topics"] = topics;
  information["channels"] = channels;

  YAML::Node root;
  root["ibmw_bagfile_information"] = information;
  std::ofstream output(options_.output_path / "metadata.yaml");
  MW_ASSERT(output.is_open(), "Failed to open metadata.yaml for writing.");
  output << root;
  output.close();
  MW_ASSERT(output.good(), "Failed to write metadata.yaml.");
}

}  // namespace ibmw::extensions::record_playback_extension
