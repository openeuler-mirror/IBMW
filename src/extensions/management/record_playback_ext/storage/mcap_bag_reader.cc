// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/mcap_bag_reader.h"
#include "record_playback_ext/storage/path_safety.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <queue>
#include <set>

#include "base/exception.h"
#include "mcap/reader.hpp"
#include "record_playback_ext/storage/compression_codec.h"
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

std::string ChannelIdentity(
    std::string_view topic,
    std::string_view msg_type,
    std::string_view serialization_type,
    std::string_view schema_hash) {
  return std::string(topic) + '\0' + std::string(msg_type) + '\0' +
         std::string(serialization_type) + '\0' + std::string(schema_hash);
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

std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(
    std::string data) {
  auto storage = std::make_shared<std::string>(std::move(data));
  auto* view = new ibmw::util::BufferArrayView(
      storage->data(), storage->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view,
      [storage](const ibmw::util::BufferArrayView* ptr) { delete ptr; });
}

class MemoryReader final : public mcap::IReadable {
 public:
  explicit MemoryReader(std::string data) : data_(std::move(data)) {}

  uint64_t size() const override { return data_.size(); }

  uint64_t read(
      std::byte** output,
      uint64_t offset,
      uint64_t size) override {
    if (offset >= data_.size()) {
      return 0;
    }
    const uint64_t available = data_.size() - offset;
    const uint64_t count = std::min(size, available);
    *output = reinterpret_cast<std::byte*>(data_.data() + offset);
    return count;
  }

 private:
  std::string data_;
};

std::unique_ptr<MemoryReader> OpenMcap(
    const std::filesystem::path& path,
    std::string_view compression_mode,
    std::string_view compression_format,
    mcap::McapReader& reader) {
  if (compression_mode == "file" &&
      path.extension() == "." + std::string(compression_format)) {
    auto memory = std::make_unique<MemoryReader>(
        ReadCompressedFile(path, compression_format));
    const auto result = reader.open(*memory);
    MW_ASSERT(
        result.ok(), "Cannot open compressed MCAP '{}': {}.", path.string(),
        result.message);
    return memory;
  }
  const auto result = reader.open(path.string());
  MW_ASSERT(
      result.ok(), "Cannot open MCAP '{}': {}.", path.string(),
      result.message);
  return nullptr;
}

void LoadMcapReaderConfiguration(
    const std::filesystem::path& path,
    std::string& summary_method,
    std::string& read_order) {
  summary_method = "allow_fallback_scan";
  read_order = "auto";
  if (path.empty()) {
    return;
  }
  const auto root = YAML::LoadFile(path.string());
  MW_ASSERT(root && root.IsMap(), "MCAP reader config must be a map.");
  for (const auto& entry : root) {
    MW_ASSERT(
        entry.first.IsScalar(), "MCAP reader config keys must be strings.");
    const std::string key = entry.first.as<std::string>();
    MW_ASSERT(
        key == "summary_method" || key == "read_order",
        "Unknown MCAP reader config key '{}'. Supported keys are "
        "summary_method and read_order.",
        key);
    MW_ASSERT(
        entry.second.IsScalar(),
        "MCAP reader config '{}' must be a scalar string.", key);
    const std::string value = entry.second.as<std::string>();
    if (key == "summary_method") {
      MW_ASSERT(
          value == "no_fallback_scan" ||
              value == "allow_fallback_scan" || value == "force_scan",
          "Invalid MCAP summary_method '{}'.", value);
      summary_method = value;
    } else {
      MW_ASSERT(
          value == "auto" || value == "file" || value == "log_time",
          "Invalid MCAP read_order '{}'.", value);
      read_order = value;
    }
  }
}

mcap::ReadSummaryMethod McapSummaryMethod(std::string_view value) {
  if (value == "no_fallback_scan") {
    return mcap::ReadSummaryMethod::NoFallbackScan;
  }
  if (value == "force_scan") {
    return mcap::ReadSummaryMethod::ForceScan;
  }
  return mcap::ReadSummaryMethod::AllowFallbackScan;
}

bool MetadataFileMatches(
    const YAML::Node& metadata,
    const BagFileInfo& actual) {
  return !metadata || !metadata.IsMap() ||
         (metadata["size"].as<uint64_t>(actual.size) == actual.size &&
          metadata["message_count"].as<uint64_t>(actual.message_count) ==
              actual.message_count &&
          metadata["start_timestamp"].as<uint64_t>(
              actual.start_timestamp) == actual.start_timestamp &&
          metadata["end_timestamp"].as<uint64_t>(actual.end_timestamp) ==
              actual.end_timestamp);
}

}  // namespace

void McapBagReader::Open(
    const std::filesystem::path& bag_path,
    const ReaderOpenOptions& options) {
  info_ = {};
  file_channels_.clear();
  file_payload_compressions_.clear();
  LoadMcapReaderConfiguration(
      options.storage_config_file, summary_method_, read_order_);

  const auto absolute_path = std::filesystem::absolute(bag_path);
  const bool direct_file = std::filesystem::is_regular_file(absolute_path);
  YAML::Node metadata;
  if (!direct_file) {
    MW_ASSERT(
        std::filesystem::is_directory(absolute_path),
        "Bag path '{}' is neither a directory nor an MCAP file.",
        absolute_path.string());
    info_.bag_path = std::filesystem::canonical(absolute_path);
    const auto metadata_path = info_.bag_path / "metadata.yaml";
    MW_ASSERT(
        std::filesystem::is_regular_file(metadata_path),
        "Bag metadata '{}' does not exist.", metadata_path.string());
    const auto metadata_root = YAML::LoadFile(metadata_path.string());
    metadata = metadata_root["ibmw_bagfile_information"];
    MW_ASSERT(metadata && metadata.IsMap(), "Invalid IBMW bag metadata.");
    info_.version = metadata["version"].as<uint32_t>();
    info_.storage_id = metadata["storage_id"].as<std::string>();
    info_.closed_cleanly = metadata["closed_cleanly"].as<bool>(false);
    info_.error_message =
        metadata["error_message"].as<std::string>("");
    info_.dropped_message_count =
        metadata["dropped_message_count"].as<uint64_t>(0);
    info_.duplicate_message_count =
        metadata["duplicate_message_count"].as<uint64_t>(0);
    info_.compression_mode =
        metadata["compression_mode"].as<std::string>("none");
    info_.compression_format =
        metadata["compression_format"].as<std::string>("");
  } else {
    const std::string filename = absolute_path.filename().string();
    MW_ASSERT(
        absolute_path.extension() == ".mcap" ||
            filename.ends_with(".mcap.zstd") ||
            filename.ends_with(".mcap.lz4"),
        "Bag path '{}' is neither a directory nor an MCAP file.",
        absolute_path.string());
    info_.bag_path = std::filesystem::canonical(absolute_path);
    info_.version = 1;
    info_.storage_id = "mcap";
    info_.closed_cleanly = true;
    if (filename.ends_with(".mcap.zstd")) {
      info_.compression_mode = "file";
      info_.compression_format = "zstd";
    } else if (filename.ends_with(".mcap.lz4")) {
      info_.compression_mode = "file";
      info_.compression_format = "lz4";
    }
  }
  MW_ASSERT(
      options.expected_storage_id.empty() ||
          options.expected_storage_id == info_.storage_id,
      "Requested storage '{}' does not match bag storage '{}'.",
      options.expected_storage_id, info_.storage_id);
  MW_ASSERT(
      info_.storage_id == "mcap",
      "Storage '{}' is not supported by the MCAP reader.", info_.storage_id);

  std::unordered_map<uint16_t, YAML::Node> metadata_channels;
  std::unordered_map<std::string, YAML::Node> metadata_channels_by_identity;
  if (const auto channels = metadata["channels"];
      channels && channels.IsSequence()) {
    for (const auto& channel : channels) {
      metadata_channels.emplace(channel["id"].as<uint16_t>(), channel);
      metadata_channels_by_identity.emplace(
          ChannelIdentity(
              channel["topic"].as<std::string>(),
              channel["msg_type"].as<std::string>(),
              channel["serialization_type"].as<std::string>(),
              channel["schema_hash"].as<std::string>("")),
          channel);
    }
  }

  std::vector<std::filesystem::path> files;
  std::vector<YAML::Node> file_metadata;
  if (direct_file) {
    files.emplace_back(std::filesystem::canonical(absolute_path));
    file_metadata.emplace_back();
  } else {
    const auto file_nodes = metadata["files"];
    MW_ASSERT(file_nodes && file_nodes.IsSequence(), "Bag has no file list.");
    for (const auto& file : file_nodes) {
      const auto path = SafeBagFilePath(
          info_.bag_path, file["path"].as<std::string>());
      MW_ASSERT(
          std::filesystem::is_regular_file(path),
          "Bag data file '{}' does not exist.", path.string());
      files.emplace_back(std::filesystem::canonical(path));
      file_metadata.emplace_back(file);
    }
  }
  MW_ASSERT(!files.empty(), "Bag contains no MCAP files.");

  std::unordered_map<std::string, size_t> schema_indices;
  std::unordered_map<std::string, size_t> channel_indices;
  for (size_t file_index = 0; file_index < files.size(); ++file_index) {
    mcap::McapReader reader;
    [[maybe_unused]] auto memory_reader = OpenMcap(
        files[file_index], info_.compression_mode,
        info_.compression_format, reader);
    const auto summary_result =
        reader.readSummary(McapSummaryMethod(summary_method_));
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
        .size = std::filesystem::file_size(files[file_index]),
        .compression =
            file_metadata[file_index] && file_metadata[file_index].IsMap()
                ? file_metadata[file_index]["compression"].as<std::string>(
                      "none")
                : (info_.compression_mode == "none"
                       ? "none"
                       : info_.compression_format),
        .split_reason =
            file_metadata[file_index] && file_metadata[file_index].IsMap()
                ? file_metadata[file_index]["split_reason"].as<std::string>(
                      "")
                : ""};
    info_.closed_cleanly =
        info_.closed_cleanly &&
        MetadataFileMatches(file_metadata[file_index], file);
    const bool had_messages = info_.message_count != 0;
    info_.files.emplace_back(file);
    info_.message_count += file.message_count;
    info_.total_size += file.size;
    if (file.message_count != 0) {
      if (!had_messages) {
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
      MW_ASSERT(id <= std::numeric_limits<uint16_t>::max(),
                "MCAP schema ID {} is outside uint16 range.", id);
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
      MW_ASSERT(id <= std::numeric_limits<uint16_t>::max() &&
                    channel->schemaId <= std::numeric_limits<uint16_t>::max(),
                "MCAP channel/schema ID exceeds uint16 range.");
      const auto schema = file_schemas.find(
          static_cast<uint16_t>(channel->schemaId));
      MW_ASSERT(
          schema != file_schemas.end(),
          "Channel '{}' references missing Schema {}.", channel->topic,
          channel->schemaId);
      const std::string msg_type = MetadataValue(
          channel->metadata, "ibmw_msg_type", schema->second.descriptor.schema_name);
      const std::string serialization = SerializationType(*channel);
      const std::string advertised_hash = MetadataValue(
          channel->metadata, "schema_hash",
          schema->second.descriptor.schema_hash);
      auto metadata_channel = metadata_channels_by_identity.find(
          ChannelIdentity(
              channel->topic, msg_type, serialization, advertised_hash));
      if (metadata_channel == metadata_channels_by_identity.end()) {
        const auto by_id = metadata_channels.find(static_cast<uint16_t>(id));
        if (by_id != metadata_channels.end()) {
          metadata_channel = metadata_channels_by_identity.emplace(
              ChannelIdentity(
                  channel->topic, msg_type, serialization, advertised_hash),
              by_id->second).first;
        }
      }
      auto descriptor = schema->second.descriptor;
      descriptor.type_name = msg_type;
      descriptor.serialization_type = serialization;
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
          .message_count =
              statistics->channelMessageCounts.contains(id)
                  ? statistics->channelMessageCounts.at(id)
                  : 0};
      for (const auto& [key, value] : channel->metadata) {
        if (key.starts_with("qos.")) {
          channel_info.topic.qos.policies.emplace(key.substr(4), value);
        }
      }
      if (metadata_channel != metadata_channels_by_identity.end()) {
        if (const auto qos = metadata_channel->second["qos"];
            qos && qos.IsMap()) {
          for (const auto& policy : qos) {
            channel_info.topic.qos.policies[
                policy.first.as<std::string>()] =
                policy.second.as<std::string>();
          }
        }
      }
      file_channels_.emplace(
          FileChannelKey{
              .file_index = file_index,
              .channel_id = static_cast<uint16_t>(id)},
          channel_info);
      const std::string payload_compression = MetadataValue(
          channel->metadata, "ibmw_payload_compression");
      if (!payload_compression.empty()) {
        MW_ASSERT(
            payload_compression == "zstd" ||
                payload_compression == "lz4",
            "Channel '{}' uses unsupported payload compression '{}'.",
            channel->topic, payload_compression);
        file_payload_compressions_.emplace(
            FileChannelKey{
                .file_index = file_index,
                .channel_id = static_cast<uint16_t>(id)},
            payload_compression);
      }
      const std::string channel_key =
          channel_info.topic.topic + '\0' + channel_info.topic.msg_type + '\0' +
          channel_info.topic.serialization_type + '\0' +
          channel_info.topic.schema.schema_hash;
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
  if (metadata && metadata.IsMap()) {
    info_.closed_cleanly =
        info_.closed_cleanly &&
        metadata["message_count"].as<uint64_t>(info_.message_count) ==
            info_.message_count &&
        metadata["start_timestamp"].as<uint64_t>(info_.start_timestamp) ==
            info_.start_timestamp &&
        metadata["end_timestamp"].as<uint64_t>(info_.end_timestamp) ==
            info_.end_timestamp;
  }
}

void McapBagReader::ReadMessages(
    const BagReadOptions& options,
    const MessageCallback& callback) const {
  MW_ASSERT(callback, "Bag message callback cannot be empty.");
  const std::set<std::string> selected_topics(
      options.topics.begin(), options.topics.end());

  struct FileCursor {
    size_t file_index = 0;
    std::unique_ptr<MemoryReader> memory_reader;
    mcap::McapReader reader;
    std::unique_ptr<mcap::LinearMessageView> messages;
    std::optional<mcap::LinearMessageView::Iterator> iterator;
    std::optional<mcap::Status> read_error;
    std::optional<BagMessage> current;
  };
  struct PendingMessage {
    uint64_t log_time = 0;
    size_t file_index = 0;
    size_t cursor_index = 0;
  };
  const auto later_message = [](
      const PendingMessage& left,
      const PendingMessage& right) {
    if (left.log_time != right.log_time) {
      return left.log_time > right.log_time;
    }
    return left.file_index > right.file_index;
  };
  std::priority_queue<
      PendingMessage,
      std::vector<PendingMessage>,
      decltype(later_message)>
      pending(later_message);
  std::vector<std::unique_ptr<FileCursor>> cursors;
  cursors.reserve(info_.files.size());

  const auto load_current = [&](FileCursor& cursor) {
    MW_ASSERT(
        cursor.messages && cursor.iterator,
        "MCAP file cursor is not initialized.");
    if (*cursor.iterator == cursor.messages->end()) {
      MW_ASSERT(
          !cursor.read_error,
          "Cannot read MCAP messages from '{}': {}.",
          info_.files[cursor.file_index].path.string(),
          cursor.read_error
              ? cursor.read_error->message
              : "unknown error");
      cursor.current.reset();
      return false;
    }

    const auto& view = **cursor.iterator;
    MW_ASSERT(view.message.channelId <= std::numeric_limits<uint16_t>::max(),
              "MCAP message channel ID exceeds uint16 range.");
    const auto channel = file_channels_.find(FileChannelKey{
        .file_index = cursor.file_index,
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
    const auto payload_compression = file_payload_compressions_.find(
        FileChannelKey{
            .file_index = cursor.file_index,
            .channel_id = static_cast<uint16_t>(view.message.channelId)});
    if (payload_compression != file_payload_compressions_.end()) {
      message.payload = MakeOwnedPayload(DecompressBuffer(
          std::string_view(
              reinterpret_cast<const char*>(view.message.data),
              static_cast<size_t>(view.message.dataSize)),
          payload_compression->second));
    }
    cursor.current.emplace(std::move(message));
    return true;
  };

  for (size_t file_index = 0; file_index < info_.files.size(); ++file_index) {
    auto cursor = std::make_unique<FileCursor>();
    cursor->file_index = file_index;
    cursor->memory_reader = OpenMcap(
        info_.files[file_index].path, info_.compression_mode,
        info_.compression_format, cursor->reader);
    const auto summary_result =
        cursor->reader.readSummary(McapSummaryMethod(summary_method_));
    MW_ASSERT(
        summary_result.ok(), "Cannot read MCAP summary for playback: {}.",
        summary_result.message);
    mcap::ReadMessageOptions read_options;
    read_options.startTime = options.start_timestamp;
    read_options.endTime =
        options.end_timestamp == 0 ? mcap::MaxTime : options.end_timestamp;
    const bool has_message_indexes =
        !cursor->reader.chunkIndexes().empty() &&
        std::all_of(
            cursor->reader.chunkIndexes().begin(),
            cursor->reader.chunkIndexes().end(),
            [](const auto& chunk) {
              return !chunk.messageIndexOffsets.empty();
            });
    if (read_order_ == "file") {
      read_options.readOrder =
          mcap::ReadMessageOptions::ReadOrder::FileOrder;
    } else if (read_order_ == "log_time") {
      read_options.readOrder =
          mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
    } else {
      read_options.readOrder =
          has_message_indexes
              ? mcap::ReadMessageOptions::ReadOrder::LogTimeOrder
              : mcap::ReadMessageOptions::ReadOrder::FileOrder;
    }
    if (!selected_topics.empty()) {
      read_options.topicFilter = [&selected_topics](std::string_view topic) {
        return selected_topics.contains(std::string(topic));
      };
    }
    auto* cursor_ptr = cursor.get();
    cursor->messages = std::make_unique<mcap::LinearMessageView>(
        cursor->reader.readMessages(
            [cursor_ptr](const mcap::Status& status) {
              if (!status.ok() && !cursor_ptr->read_error) {
                cursor_ptr->read_error = status;
              }
            },
            read_options));
    cursor->iterator.emplace(cursor->messages->begin());
    const size_t cursor_index = cursors.size();
    if (load_current(*cursor)) {
      pending.push(PendingMessage{
          .log_time = cursor->current->log_time,
          .file_index = file_index,
          .cursor_index = cursor_index});
    }
    cursors.emplace_back(std::move(cursor));
  }

  while (!pending.empty()) {
    const auto next = pending.top();
    pending.pop();
    auto& cursor = *cursors[next.cursor_index];
    MW_ASSERT(cursor.current, "MCAP file cursor has no pending message.");
    auto message = std::move(*cursor.current);
    cursor.current.reset();
    if (!callback(std::move(message))) {
      return;
    }
    ++(*cursor.iterator);
    if (load_current(cursor)) {
      pending.push(PendingMessage{
          .log_time = cursor.current->log_time,
          .file_index = cursor.file_index,
          .cursor_index = next.cursor_index});
    }
  }
}

}  // namespace ibmw::extensions::record_playback_extension
