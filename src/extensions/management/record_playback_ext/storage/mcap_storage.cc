// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/mcap_storage.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <string_view>

#include "base/exception.h"
#include "record_playback_ext/storage/compression_codec.h"
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

int CompressionLevel(
    std::string_view format,
    std::string_view level) {
  if (format == "zstd") {
    if (level == "fastest") return 1;
    if (level == "fast") return 2;
    if (level == "default") return 0;
    if (level == "slow") return 10;
    if (level == "slowest") return 19;
  } else if (format == "lz4") {
    if (level == "fastest" || level == "fast" || level == "default") {
      return 0;
    }
    if (level == "slow") return 9;
    if (level == "slowest") return 12;
  }
  MW_ASSERT(false, "Invalid MCAP compression level '{}'.", level);
  return 0;
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
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_mode == "file" ||
          options.compression_mode == "message",
      "Unsupported MCAP compression mode '{}'.", options.compression_mode);
  MW_ASSERT(
      options.compression_mode == "none" ||
          options.compression_format == "zstd" ||
          options.compression_format == "lz4",
      "MCAP compression requires zstd or lz4.");
  MW_ASSERT(
      options.compression_mode == "none" ||
          (options.compression_queue_size > 0 &&
           options.compression_threads > 0),
      "MCAP compression requires a non-zero queue and worker count.");

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

  summary_.output_path = options_.output_path;
  if (options_.compression_mode != "none") {
    compression_pool_ = std::make_unique<CompressionTaskPool>(
        options_.compression_threads);
  }
  state_ = State::kOpen;
  OpenNewFile();
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

  const auto id = next_schema_id_++;
  schemas_by_key_.emplace(
      key, StoredSchema{.id = id, .descriptor = schema});
  schemas_by_id_.emplace(id, schema);
  mcap::Schema mcap_schema(
      schema.schema_name, schema.schema_encoding, schema.schema_data);
  writer_->addSchema(mcap_schema);
  MW_ASSERT(mcap_schema.id != 0, "MCAP returned an invalid Schema ID.");
  current_schema_ids_.emplace(id, mcap_schema.id);
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
  if (options_.compression_mode == "message") {
    metadata.emplace("ibmw_payload_compression", options_.compression_format);
  }
  for (const auto& [policy, value] : channel.topic.qos.policies) {
    metadata.emplace("qos." + policy, value);
  }
  const auto current_schema = current_schema_ids_.find(schema_id);
  MW_ASSERT(
      current_schema != current_schema_ids_.end(),
      "Schema ID {} is unavailable in the current MCAP file.", schema_id);
  mcap::Channel mcap_channel(
      channel.topic.topic,
      MessageEncoding(channel.topic.serialization_type),
      current_schema->second, metadata);
  writer_->addChannel(mcap_channel);
  MW_ASSERT(mcap_channel.id != 0, "MCAP returned an invalid Channel ID.");

  const auto id = next_channel_id_++;
  StoredChannel stored{
      .id = id, .schema_id = schema_id, .descriptor = channel};
  channels_by_key_.emplace(key, stored);
  channels_by_id_.emplace(id, std::move(stored));
  current_channel_ids_.emplace(id, mcap_channel.id);
  summary_.channel_message_counts.emplace(id, 0);
  return id;
}

void McapStorage::Write(
    StorageChannelId channel_id,
    const ibmw::transport::SerializedMessage& message) {
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

  const std::string payload = message.payload->JoinToString();
  if (ShouldSplit(log_time, payload.size())) {
    const std::string reason =
        options_.max_bag_size > 0 &&
                current_file_estimated_size_ + payload.size() + 64 >
                    options_.max_bag_size
            ? "size"
            : "duration";
    current_split_reason_ = reason;
    CloseCurrentFile();
    OpenNewFile();
  }
  const auto current_channel = current_channel_ids_.find(channel_id);
  MW_ASSERT(
      current_channel != current_channel_ids_.end(),
      "Channel ID {} is unavailable in the current MCAP file.", channel_id);
  MW_ASSERT(
      message.identity.sequence <= std::numeric_limits<uint32_t>::max(),
      "Message sequence {} exceeds MCAP uint32 range.",
      message.identity.sequence);
  const auto sequence = static_cast<uint32_t>(message.identity.sequence);
  if (options_.compression_mode == "message") {
    while (pending_messages_.size() >= options_.compression_queue_size) {
      DrainPendingMessage();
    }
    const std::string format = options_.compression_format;
    const int level = compression_level_;
    pending_messages_.emplace_back(PendingMessage{
        .channel_id = current_channel->second,
        .sequence = sequence,
        .log_time = log_time,
        .publish_time = publish_time,
        .payload = compression_pool_->Submit(
            [payload, format, level] {
              return CompressBuffer(payload, format, level);
            })});
  } else {
    WriteMcapMessage(
        current_channel->second, sequence, log_time, publish_time, payload);
  }
  AccountMessage(channel_id, log_time, payload.size());
}

void McapStorage::WriteMcapMessage(
    mcap::ChannelId channel_id,
    uint32_t sequence,
    uint64_t log_time,
    uint64_t publish_time,
    const std::string& payload) {
  mcap::Message mcap_message{
      .channelId = channel_id,
      .sequence = sequence,
      .logTime = log_time,
      .publishTime = publish_time,
      .dataSize = static_cast<uint64_t>(payload.size()),
      .data = reinterpret_cast<const std::byte*>(payload.data())};
  const auto result = writer_->write(mcap_message);
  MW_ASSERT(result.ok(), "Failed to write MCAP message: {}.", result.message);
}

void McapStorage::AccountMessage(
    StorageChannelId channel_id,
    uint64_t log_time,
    size_t payload_size) {
  if (summary_.message_count == 0) {
    summary_.start_timestamp = log_time;
    summary_.end_timestamp = log_time;
  } else {
    summary_.start_timestamp = std::min(summary_.start_timestamp, log_time);
    summary_.end_timestamp = std::max(summary_.end_timestamp, log_time);
  }
  ++summary_.message_count;
  ++summary_.channel_message_counts[channel_id];
  if (current_file_message_count_ == 0) {
    current_file_start_timestamp_ = log_time;
    current_file_end_timestamp_ = log_time;
  } else {
    current_file_start_timestamp_ =
        std::min(current_file_start_timestamp_, log_time);
    current_file_end_timestamp_ =
        std::max(current_file_end_timestamp_, log_time);
  }
  ++current_file_message_count_;
  current_file_estimated_size_ += payload_size + 64;
}

void McapStorage::DrainPendingMessage() {
  MW_ASSERT(!pending_messages_.empty(), "No pending message to drain.");
  auto pending = std::move(pending_messages_.front());
  pending_messages_.pop_front();
  WriteMcapMessage(
      pending.channel_id, pending.sequence, pending.log_time,
      pending.publish_time, pending.payload.get());
}

void McapStorage::DrainPendingMessages() {
  while (!pending_messages_.empty()) {
    DrainPendingMessage();
  }
}

void McapStorage::Flush() {
  MW_ASSERT(state_ == State::kOpen, "Storage is not open.");
  DrainPendingMessages();
  DrainPendingFiles();
  writer_->closeLastChunk();
  auto* sink = writer_->dataSink();
  MW_ASSERT(sink != nullptr, "MCAP writer has no output sink.");
  sink->flush();
}

void McapStorage::Close() {
  if (state_ != State::kOpen) {
    return;
  }

  CloseCurrentFile();
  DrainPendingFiles();
  if (compression_pool_) {
    compression_pool_->Shutdown();
  }
  summary_.closed_cleanly = requested_clean_close_;
  WriteMetadata();
  state_ = State::kClosed;
}

void McapStorage::SetMessageStatistics(
    uint64_t dropped_message_count,
    uint64_t duplicate_message_count) {
  summary_.dropped_message_count = dropped_message_count;
  summary_.duplicate_message_count = duplicate_message_count;
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

void McapStorage::OpenNewFile(std::string split_reason) {
  mcap_path_ = options_.output_path /
               ("data_" + std::to_string(file_index_++) + ".mcap");
  writer_ = std::make_unique<mcap::McapWriter>();
  auto writer_options = mcap::McapWriterOptions("ibmw");
  writer_options.compression = Compression();
  compression_level_ = 0;
  if (options_.storage_preset_profile == "fastwrite") {
    writer_options.noChunkCRC = true;
    writer_options.noSummaryCRC = true;
    writer_options.compressionLevel = mcap::CompressionLevel::Fastest;
    if (options_.compression_mode != "none") {
      compression_level_ = CompressionLevel(
          options_.compression_format, "fastest");
    }
  } else if (options_.storage_preset_profile == "small") {
    writer_options.compressionLevel = mcap::CompressionLevel::Slow;
    writer_options.forceCompression = true;
    if (options_.compression_mode != "none") {
      compression_level_ = CompressionLevel(
          options_.compression_format, "slow");
    }
  }
  if (!options_.storage_config_file.empty()) {
    const auto config = YAML::LoadFile(options_.storage_config_file.string());
    if (config["chunk_size"]) {
      writer_options.chunkSize = config["chunk_size"].as<uint64_t>();
    }
    if (config["no_chunking"]) {
      writer_options.noChunking = config["no_chunking"].as<bool>();
    }
    if (config["force_compression"]) {
      writer_options.forceCompression =
          config["force_compression"].as<bool>();
    }
    if (config["compression_level"]) {
      const std::string level =
          config["compression_level"].as<std::string>();
      static const std::unordered_map<
          std::string,
          mcap::CompressionLevel>
          levels{{"fastest", mcap::CompressionLevel::Fastest},
                 {"fast", mcap::CompressionLevel::Fast},
                 {"default", mcap::CompressionLevel::Default},
                 {"slow", mcap::CompressionLevel::Slow},
                 {"slowest", mcap::CompressionLevel::Slowest}};
      const auto found = levels.find(level);
      MW_ASSERT(found != levels.end(), "Invalid MCAP compression level.");
      writer_options.compressionLevel = found->second;
      if (options_.compression_mode != "none") {
        compression_level_ = CompressionLevel(
            options_.compression_format, level);
      }
    }
  }
  const auto result = writer_->open(mcap_path_.string(), writer_options);
  MW_ASSERT(
      result.ok(), "Failed to open MCAP '{}': {}.", mcap_path_.string(),
      result.message);
  current_file_start_timestamp_ = 0;
  current_file_end_timestamp_ = 0;
  current_file_message_count_ = 0;
  current_file_estimated_size_ = 0;
  current_split_reason_ = std::move(split_reason);
  current_schema_ids_.clear();
  current_channel_ids_.clear();

  for (const auto& [id, schema] : schemas_by_id_) {
    mcap::Schema mcap_schema(
        schema.schema_name, schema.schema_encoding, schema.schema_data);
    writer_->addSchema(mcap_schema);
    current_schema_ids_.emplace(id, mcap_schema.id);
  }
  for (const auto& [id, channel] : channels_by_id_) {
    const auto schema = schemas_by_id_.find(channel.schema_id);
    const auto current_schema = current_schema_ids_.find(channel.schema_id);
    MW_ASSERT(
        schema != schemas_by_id_.end() &&
            current_schema != current_schema_ids_.end(),
        "Cannot restore Channel {} in a split MCAP file.", id);
    mcap::KeyValueMap metadata{
        {"ibmw_msg_type", channel.descriptor.topic.msg_type},
        {"serialization_type", channel.descriptor.topic.serialization_type},
        {"schema_hash", schema->second.schema_hash}};
    if (options_.compression_mode == "message") {
      metadata.emplace(
          "ibmw_payload_compression", options_.compression_format);
    }
    for (const auto& [policy, value] :
         channel.descriptor.topic.qos.policies) {
      metadata.emplace("qos." + policy, value);
    }
    mcap::Channel mcap_channel(
        channel.descriptor.topic.topic,
        MessageEncoding(channel.descriptor.topic.serialization_type),
        current_schema->second, metadata);
    writer_->addChannel(mcap_channel);
    current_channel_ids_.emplace(id, mcap_channel.id);
  }
}

void McapStorage::CloseCurrentFile() {
  if (!writer_) {
    return;
  }
  DrainPendingMessages();
  writer_->close();
  writer_.reset();
  StorageFileSummary file_summary{
      .relative_path = mcap_path_.filename(),
      .start_timestamp = current_file_start_timestamp_,
      .end_timestamp = current_file_end_timestamp_,
      .message_count = current_file_message_count_,
      .file_size = 0,
      .compression =
          options_.compression_mode == "none"
              ? "none"
              : options_.compression_format,
      .split_reason = current_split_reason_};
  if (options_.compression_mode == "file") {
    while (pending_files_.size() >= options_.compression_queue_size) {
      DrainPendingFile();
    }
    const auto input_path = mcap_path_;
    const std::string format = options_.compression_format;
    const int level = compression_level_;
    pending_files_.emplace_back(PendingFile{
        .summary = std::move(file_summary),
        .output_path = compression_pool_->Submit(
            [input_path, format, level] {
              return CompressFile(input_path, format, level).string();
            })});
    return;
  }

  file_summary.file_size = std::filesystem::file_size(mcap_path_);
  summary_.file_size += file_summary.file_size;
  summary_.relative_files.emplace_back(file_summary.relative_path);
  summary_.files.emplace_back(std::move(file_summary));
}

void McapStorage::DrainPendingFile() {
  MW_ASSERT(!pending_files_.empty(), "No pending file to drain.");
  auto pending = std::move(pending_files_.front());
  pending_files_.pop_front();
  const std::filesystem::path output_path = pending.output_path.get();
  pending.summary.relative_path = output_path.filename();
  pending.summary.file_size = std::filesystem::file_size(output_path);
  summary_.file_size += pending.summary.file_size;
  summary_.relative_files.emplace_back(pending.summary.relative_path);
  summary_.files.emplace_back(std::move(pending.summary));
}

void McapStorage::DrainPendingFiles() {
  while (!pending_files_.empty()) {
    DrainPendingFile();
  }
}

bool McapStorage::ShouldSplit(
    uint64_t log_time,
    size_t payload_size) const {
  if (current_file_message_count_ == 0) {
    return false;
  }
  if (options_.max_bag_size > 0 &&
      current_file_estimated_size_ + payload_size + 64 >
          options_.max_bag_size) {
    return true;
  }
  return options_.max_bag_duration_ns > 0 &&
         log_time >= current_file_start_timestamp_ &&
         log_time - current_file_start_timestamp_ >=
             options_.max_bag_duration_ns;
}

mcap::Compression McapStorage::Compression() const {
  return mcap::Compression::None;
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
  information["dropped_message_count"] = summary_.dropped_message_count;
  information["duplicate_message_count"] =
      summary_.duplicate_message_count;
  information["compression_mode"] = options_.compression_mode;
  information["compression_format"] = options_.compression_format;
  information["compression_queue_size"] = options_.compression_queue_size;
  information["compression_threads"] = options_.compression_threads;

  YAML::Node files(YAML::NodeType::Sequence);
  for (const auto& summary_file : summary_.files) {
    YAML::Node file;
    file["path"] = summary_file.relative_path.string();
    file["start_timestamp"] = summary_file.start_timestamp;
    file["end_timestamp"] = summary_file.end_timestamp;
    file["message_count"] = summary_file.message_count;
    file["size"] = summary_file.file_size;
    file["compression"] = summary_file.compression;
    file["split_reason"] = summary_file.split_reason;
    files.push_back(file);
  }
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
    item["schema_hash"] =
        channel.descriptor.topic.schema.schema_hash;
    item["message_count"] = summary_.channel_message_counts.at(id);
    YAML::Node qos;
    for (const auto& [policy, value] :
         channel.descriptor.topic.qos.policies) {
      qos[policy] = value;
    }
    item["qos"] = qos;
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
