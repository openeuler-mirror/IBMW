// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/converter/bag_converter.h"

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>

#include <sqlite3.h>

#include "base/exception.h"
#include "mcap/reader.hpp"
#include "record_playback_ext/converter/rosbag2_adapter.h"
#include "record_playback_ext/storage/bag_reader_factory.h"
#include "record_playback_ext/storage/storage_factory.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

struct RegisteredChannel {
  StorageChannelId id = 0;
  ibmw::transport::TopicDescriptor topic;
};

std::string ChannelKey(const BagChannelInfo& channel) {
  return channel.topic.topic + '\0' + channel.topic.msg_type + '\0' +
         channel.topic.serialization_type + '\0' +
         channel.topic.schema.schema_hash;
}

StorageChannelDescriptor ToStorageChannel(const BagChannelInfo& channel) {
  return StorageChannelDescriptor{.topic = channel.topic};
}

std::string CurrentExceptionMessage(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& exception) {
    return exception.what();
  } catch (...) {
    return "unknown conversion error";
  }
}

std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
  return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

std::string DetectMcapFormat(const std::filesystem::path& path) {
  mcap::McapReader reader;
  const auto result = reader.open(path.string());
  MW_ASSERT(result.ok(), "Cannot open MCAP '{}': {}.", path.string(),
            result.message);
  MW_ASSERT(reader.header(), "MCAP '{}' has no Header.", path.string());
  const std::string format = reader.header()->profile == "ros2"
                                 ? "rosbag2"
                                 : "ibmw";
  reader.close();
  return format;
}

std::string DetectSqliteFormat(const std::filesystem::path& path) {
  sqlite3* database = nullptr;
  MW_ASSERT(
      sqlite3_open_v2(
          path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
      "Cannot open SQLite bag '{}'.", path.string());
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> guard(
      database, &sqlite3_close_v2);
  const auto has_table = [database](std::string_view table) {
    sqlite3_stmt* statement = nullptr;
    MW_ASSERT(
        sqlite3_prepare_v2(
            database,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1,
            &statement, nullptr) == SQLITE_OK,
        "Cannot inspect SQLite bag schema.");
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>
        statement_guard(statement, &sqlite3_finalize);
    MW_ASSERT(
        sqlite3_bind_text(
            statement, 1, table.data(), static_cast<int>(table.size()),
            SQLITE_TRANSIENT) == SQLITE_OK,
        "Cannot inspect SQLite bag table '{}'.", table);
    const int result = sqlite3_step(statement);
    MW_ASSERT(
        result == SQLITE_ROW || result == SQLITE_DONE,
        "Cannot inspect SQLite bag table '{}'.", table);
    return result == SQLITE_ROW;
  };
  if (has_table("schemas") && has_table("channels")) {
    return "ibmw";
  }
  if (has_table("topics")) {
    return "rosbag2";
  }
  MW_ASSERT(false, "Cannot identify SQLite bag ecosystem from '{}'.",
            path.string());
  return {};
}

std::string DetectBagFormat(const BagConvertOptions& options) {
  if (options.input_format != "auto") {
    return options.input_format;
  }
  const auto absolute = std::filesystem::absolute(options.input_path);
  if (!std::filesystem::is_directory(absolute)) {
    MW_ASSERT(std::filesystem::is_regular_file(absolute),
              "Bag input '{}' does not exist.", absolute.string());
    if (absolute.extension() == ".mcap") {
      return DetectMcapFormat(absolute);
    }
    if (absolute.extension() == ".db3") {
      return DetectSqliteFormat(absolute);
    }
    MW_ASSERT(false, "Cannot identify standalone bag file '{}'.",
              absolute.string());
    return {};
  }
  const auto metadata_path = absolute / "metadata.yaml";
  MW_ASSERT(std::filesystem::is_regular_file(metadata_path),
            "Bag metadata '{}' does not exist.", metadata_path.string());
  const auto root = YAML::LoadFile(metadata_path.string());
  if (root["ibmw_bagfile_information"]) {
    return "ibmw";
  }
  if (root["rosbag2_bagfile_information"]) {
    return "rosbag2";
  }
  MW_ASSERT(false, "Cannot identify bag ecosystem from '{}'.", metadata_path.string());
  return {};
}

std::unique_ptr<BagReader> OpenInput(const BagConvertOptions& options) {
  const auto input_format = DetectBagFormat(options);
  MW_ASSERT(input_format == "ibmw" || input_format == "rosbag2",
            "Input format must be auto, ibmw, or rosbag2.");
  if (input_format == "rosbag2") {
    auto reader = std::make_unique<Rosbag2BagReader>();
    reader->Open(options.input_path,
                 ReaderOpenOptions{
                     .expected_storage_id = options.input_storage_id,
                     .storage_config_file = options.input_storage_config_file});
    return reader;
  }
  return CreateBagReader(
      options.input_path,
      ReaderOpenOptions{
          .expected_storage_id = options.input_storage_id,
          .storage_config_file = options.input_storage_config_file});
}

}  // namespace

BagConvertResult ConvertBag(const BagConvertOptions& options) {
  MW_ASSERT(!options.input_path.empty(), "Bag conversion input cannot be empty.");
  MW_ASSERT(
      !options.output.output_path.empty(),
      "Bag conversion output cannot be empty.");
  MW_ASSERT(
      NormalizedPath(options.input_path) !=
          NormalizedPath(options.output.output_path),
      "Bag conversion input and output must be different paths.");

  MW_ASSERT(options.output_format == "ibmw" || options.output_format == "rosbag2",
            "Output format must be ibmw or rosbag2.");

  auto reader = OpenInput(options);
  const auto& input_info = reader->Info();

  if (options.output_format == "rosbag2") {
    return ConvertBagToRosbag2(*reader, options.output);
  }

  auto storage = CreateStorage(options.output.storage_id);
  storage->Open(options.output);

  std::unordered_map<std::string, RegisteredChannel> channel_ids;
  const auto register_channel =
      [&](const BagChannelInfo& channel) -> StorageChannelId {
    const std::string key = ChannelKey(channel);
    const auto existing = channel_ids.find(key);
    if (existing != channel_ids.end()) {
      MW_ASSERT(
          existing->second.topic.serialization_type ==
                  channel.topic.serialization_type &&
              existing->second.topic.schema.schema_hash ==
                  channel.topic.schema.schema_hash,
          "Conflicting Channel descriptor for topic '{}'.",
          channel.topic.topic);
      return existing->second.id;
    }

    const auto schema_id = storage->RegisterSchema(channel.topic.schema);
    const auto channel_id =
        storage->RegisterChannel(ToStorageChannel(channel), schema_id);
    channel_ids.emplace(
        key, RegisteredChannel{.id = channel_id, .topic = channel.topic});
    return channel_id;
  };

  try {
    for (const auto& schema : input_info.schemas) {
      static_cast<void>(storage->RegisterSchema(schema.descriptor));
    }
    for (const auto& channel : input_info.channels) {
      static_cast<void>(register_channel(channel));
    }

    reader->ReadMessages(
        {},
        [&](BagMessage&& input) {
          const auto channel_id = register_channel(input.channel);
          storage->Write(
              channel_id,
              ibmw::transport::SerializedMessage{
                  .topic = input.channel.topic,
                  .identity = {.sequence = input.sequence},
                  .publish_timestamp = input.publish_time,
                  .receive_timestamp = input.log_time,
                  .payload = std::move(input.payload)});
          return true;
        });

    MW_ASSERT(
        storage->Summary().message_count == input_info.message_count,
        "Converted message count {} does not match input message count {}.",
        storage->Summary().message_count, input_info.message_count);
    storage->SetMessageStatistics(
        input_info.dropped_message_count,
        input_info.duplicate_message_count);
    storage->SetCloseStatus(true, {});
    storage->Close();
  } catch (...) {
    const auto error = std::current_exception();
    try {
      storage->SetMessageStatistics(
          input_info.dropped_message_count,
          input_info.duplicate_message_count);
      storage->SetCloseStatus(
          false, "Bag conversion failed: " + CurrentExceptionMessage(error));
      storage->Close();
    } catch (...) {
    }
    std::rethrow_exception(error);
  }

  const auto& summary = storage->Summary();
  return BagConvertResult{
      .input_storage_id = input_info.storage_id,
      .output_storage_id = options.output.storage_id,
      .output_path = summary.output_path,
      .input_message_count = input_info.message_count,
      .output_message_count = summary.message_count,
      .schema_count = input_info.schemas.size(),
      .channel_count = channel_ids.size(),
      .input_closed_cleanly = input_info.closed_cleanly};
}

}  // namespace ibmw::extensions::record_playback_extension
