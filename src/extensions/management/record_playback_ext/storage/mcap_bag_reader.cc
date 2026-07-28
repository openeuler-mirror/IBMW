// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/mcap_bag_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

#include "base/exception.h"
#include "mcap/reader.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

std::string MetadataValue(
    const mcap::KeyValueMap& metadata,
    std::string_view key,
    std::string fallback = {}) {
  const auto found = metadata.find(std::string(key));
  return found == metadata.end() ? std::move(fallback) : found->second;
}

std::string SerializationType(const mcap::Channel& channel) {
  auto serialization = MetadataValue(
      channel.metadata, "serialization_type", channel.messageEncoding);
  if (serialization == "cdr" &&
      channel.messageEncoding == "cdr" &&
      !channel.metadata.contains("serialization_type")) {
    return "cdr";
  }
  return serialization;
}

std::vector<std::string> ReadStringSequence(const YAML::Node& node) {
  std::vector<std::string> result;
  if (!node || !node.IsSequence()) {
    return result;
  }
  for (const auto& item : node) {
    result.emplace_back(item.as<std::string>());
  }
  return result;
}

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    const std::byte* data,
    size_t size) {
  auto storage = std::make_shared<std::vector<uint8_t>>(size);
  std::memcpy(storage->data(), data, size);
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view,
      [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

}  // namespace

void McapBagReader::Open(
    const std::filesystem::path& bag_path,
    std::string_view expected_storage_id) {
  info_ = {};
  file_channels_.clear();

  const auto absolute_path = std::filesystem::absolute(bag_path);
  std::filesystem::path metadata_path;
  if (std::filesystem::is_directory(absolute_path)) {
    info_.bag_path = std::filesystem::canonical(absolute_path);
    metadata_path = info_.bag_path / "metadata.yaml";
  } else {
    MW_ASSERT(
        std::filesystem::is_regular_file(absolute_path) &&
            absolute_path.extension() == ".mcap",
        "Bag path '{}' is neither a directory nor an MCAP file.",
        absolute_path.string());
    info_.bag_path = std::filesystem::canonical(absolute_path.parent_path());
    metadata_path = info_.bag_path / "metadata.yaml";
  }
  MW_ASSERT(
      std::filesystem::is_regular_file(metadata_path),
      "Bag metadata '{}' does not exist.", metadata_path.string());

  const auto metadata_root = YAML::LoadFile(metadata_path.string());
  const auto metadata = metadata_root["ibmw_bagfile_information"];
  MW_ASSERT(metadata && metadata.IsMap(), "Invalid IBMW bag metadata.");
  info_.version = metadata["version"].as<uint32_t>();
  info_.storage_id = metadata["storage_id"].as<std::string>();
  MW_ASSERT(
      expected_storage_id.empty() || expected_storage_id == info_.storage_id,
      "Requested storage '{}' does not match bag storage '{}'.",
      expected_storage_id, info_.storage_id);
  MW_ASSERT(
      info_.storage_id == "mcap",
      "Storage '{}' is not supported by the MCAP reader.", info_.storage_id);
  info_.closed_cleanly = metadata["closed_cleanly"].as<bool>(false);
  info_.error_message = metadata["error_message"].as<std::string>("");
  info_.dropped_message_count =
      metadata["dropped_message_count"].as<uint64_t>(0);
  info_.duplicate_message_count =
      metadata["duplicate_message_count"].as<uint64_t>(0);

  std::unordered_map<uint16_t, YAML::Node> metadata_channels;
  if (const auto channels = metadata["channels"];
      channels && channels.IsSequence()) {
    for (const auto& channel : channels) {
      metadata_channels.emplace(channel["id"].as<uint16_t>(), channel);
    }
  }

  std::vector<std::filesystem::path> files;
  if (!std::filesystem::is_directory(absolute_path)) {
    files.emplace_back(std::filesystem::canonical(absolute_path));
  } else {
    const auto file_nodes = metadata["files"];
    MW_ASSERT(file_nodes && file_nodes.IsSequence(), "Bag has no file list.");
    for (const auto& file : file_nodes) {
      const auto path = info_.bag_path / file["path"].as<std::string>();
      MW_ASSERT(
          std::filesystem::is_regular_file(path),
          "Bag data file '{}' does not exist.", path.string());
      files.emplace_back(std::filesystem::canonical(path));
    }
  }
  MW_ASSERT(!files.empty(), "Bag contains no MCAP files.");

  std::unordered_map<std::string, size_t> schema_indices;
  std::unordered_map<std::string, size_t> channel_indices;
  for (size_t file_index = 0; file_index < files.size(); ++file_index) {
    mcap::McapReader reader;
    const auto open_result = reader.open(files[file_index].string());
    MW_ASSERT(
        open_result.ok(), "Cannot open MCAP '{}': {}.",
        files[file_index].string(), open_result.message);
    const auto summary_result =
        reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    MW_ASSERT(
        summary_result.ok(), "Cannot read MCAP summary '{}': {}.",
        files[file_index].string(), summary_result.message);
    info_.closed_cleanly =
        info_.closed_cleanly && reader.footer().has_value();
    const auto statistics = reader.statistics();
    MW_ASSERT(statistics.has_value(), "MCAP '{}' has no statistics.",
              files[file_index].string());

    BagFileInfo file{
        .path = files[file_index],
        .start_timestamp = statistics->messageStartTime,
        .end_timestamp = statistics->messageEndTime,
        .message_count = statistics->messageCount,
        .size = std::filesystem::file_size(files[file_index])};
    info_.files.emplace_back(file);
    info_.message_count += file.message_count;
    info_.total_size += file.size;
    if (file.message_count != 0) {
      if (info_.start_timestamp == 0) {
        info_.start_timestamp = file.start_timestamp;
        info_.end_timestamp = file.end_timestamp;
      } else {
        info_.start_timestamp =
            std::min(info_.start_timestamp, file.start_timestamp);
        info_.end_timestamp = std::max(info_.end_timestamp, file.end_timestamp);
      }
    }

    std::unordered_map<uint16_t, BagSchemaInfo> file_schemas;
    for (const auto& [id, schema] : reader.schemas()) {
      MW_ASSERT(schema != nullptr, "MCAP contains a null Schema.");
      const std::string schema_data(
          reinterpret_cast<const char*>(schema->data.data()),
          schema->data.size());
      const auto hash = ibmw::util::ComputeSchemaHash(schema_data);
      BagSchemaInfo schema_info{
          .id = static_cast<uint16_t>(id),
          .descriptor =
              {.schema_name = schema->name,
               .schema_encoding = schema->encoding,
               .schema_data = schema_data,
               .schema_hash = hash},
          .hash_valid = true};
      file_schemas.emplace(static_cast<uint16_t>(id), schema_info);
    }

    for (const auto& [id, channel] : reader.channels()) {
      MW_ASSERT(channel != nullptr, "MCAP contains a null Channel.");
      const auto schema = file_schemas.find(
          static_cast<uint16_t>(channel->schemaId));
      MW_ASSERT(
          schema != file_schemas.end(),
          "Channel '{}' references missing Schema {}.", channel->topic,
          channel->schemaId);
      const auto metadata_channel = metadata_channels.find(
          static_cast<uint16_t>(id));
      const std::string msg_type = MetadataValue(
          channel->metadata, "ibmw_msg_type", schema->second.descriptor.schema_name);
      const std::string serialization = SerializationType(*channel);
      auto descriptor = schema->second.descriptor;
      descriptor.type_name = msg_type;
      descriptor.serialization_type = serialization;
      const std::string advertised_hash = MetadataValue(
          channel->metadata, "schema_hash", descriptor.schema_hash);
      descriptor.schema_hash = advertised_hash;
      const bool hash_valid =
          advertised_hash == ibmw::util::ComputeSchemaHash(descriptor.schema_data);
      auto schema_info = schema->second;
      schema_info.descriptor = descriptor;
      schema_info.hash_valid = hash_valid;
      const std::string schema_key =
          descriptor.type_name + '\0' + descriptor.serialization_type + '\0' +
          descriptor.schema_hash;
      if (!schema_indices.contains(schema_key)) {
        schema_indices.emplace(schema_key, info_.schemas.size());
        info_.schemas.emplace_back(schema_info);
      }

      BagChannelInfo channel_info{
          .id = static_cast<uint16_t>(id),
          .schema_id = static_cast<uint16_t>(channel->schemaId),
          .topic =
              {.topic = channel->topic,
               .msg_type = msg_type,
               .serialization_type = serialization,
               .schema = descriptor},
          .source_id = MetadataValue(channel->metadata, "source_id"),
          .publisher_id = MetadataValue(channel->metadata, "publisher_id"),
          .record_driver = MetadataValue(channel->metadata, "record_driver"),
          .message_count =
              statistics->channelMessageCounts.contains(id)
                  ? statistics->channelMessageCounts.at(id)
                  : 0};
      if (metadata_channel != metadata_channels.end()) {
        channel_info.topic.available_drivers = ReadStringSequence(
            metadata_channel->second["observed_drivers"]);
      }
      file_channels_.emplace(
          FileChannelKey{
              .file_index = file_index,
              .channel_id = static_cast<uint16_t>(id)},
          channel_info);
      const std::string channel_key =
          channel_info.topic.topic + '\0' + channel_info.topic.msg_type + '\0' +
          channel_info.source_id + '\0' + channel_info.publisher_id;
      const auto existing = channel_indices.find(channel_key);
      if (existing == channel_indices.end()) {
        channel_indices.emplace(channel_key, info_.channels.size());
        info_.channels.emplace_back(std::move(channel_info));
      } else {
        info_.channels[existing->second].message_count +=
            channel_info.message_count;
      }
    }
    reader.close();
  }
}

void McapBagReader::ReadMessages(
    const BagReadOptions& options,
    const MessageCallback& callback) const {
  MW_ASSERT(callback, "Bag message callback cannot be empty.");
  const std::set<std::string> selected_topics(
      options.topics.begin(), options.topics.end());
  for (size_t file_index = 0; file_index < info_.files.size(); ++file_index) {
    mcap::McapReader reader;
    const auto result = reader.open(info_.files[file_index].path.string());
    MW_ASSERT(result.ok(), "Cannot open MCAP for playback: {}.", result.message);
    mcap::ReadMessageOptions read_options;
    read_options.startTime = options.start_timestamp;
    read_options.endTime =
        options.end_timestamp == 0 ? mcap::MaxTime : options.end_timestamp;
    read_options.readOrder =
        mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    if (!selected_topics.empty()) {
      read_options.topicFilter = [&selected_topics](std::string_view topic) {
        return selected_topics.contains(std::string(topic));
      };
    }
    for (const auto& view : reader.readMessages(
             [](const mcap::Status&) {}, read_options)) {
      const auto channel = file_channels_.find(FileChannelKey{
          .file_index = file_index,
          .channel_id = static_cast<uint16_t>(view.message.channelId)});
      MW_ASSERT(
          channel != file_channels_.end(),
          "Message references unknown Channel {}.", view.message.channelId);
      BagMessage message{
          .channel = channel->second,
          .log_time = view.message.logTime,
          .publish_time = view.message.publishTime,
          .sequence = view.message.sequence,
          .payload = MakeOwnedPayload(
              view.message.data, static_cast<size_t>(view.message.dataSize))};
      if (!callback(std::move(message))) {
        reader.close();
        return;
      }
    }
    reader.close();
  }
}

}  // namespace ibmw::extensions::record_playback_extension
