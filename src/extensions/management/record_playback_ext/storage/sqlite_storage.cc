// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/sqlite_storage.h"

#include <algorithm>
#include <fstream>
#include <limits>

#include "base/exception.h"
#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr uint32_t kBagVersion = 1;

std::string JsonQos(
    const ibmw::transport::QosDescriptor& qos) {
  return nlohmann::json(qos.policies).dump();
}

void BindText(sqlite3_stmt* statement, int index, const std::string& value) {
  MW_ASSERT(
      sqlite3_bind_text(
          statement, index, value.data(), static_cast<int>(value.size()),
          SQLITE_TRANSIENT) == SQLITE_OK,
      "Failed to bind SQLite text.");
}

}  // namespace

SqliteStorage::~SqliteStorage() {
  try {
    Close();
  } catch (...) {
  }
}

void SqliteStorage::Open(const StorageOptions& options) {
  MW_ASSERT(!open_, "SQLite storage can only be opened once.");
  MW_ASSERT(options.storage_id == "sqlite3", "SQLite storage ID must be sqlite3.");
  MW_ASSERT(
      options.compression_mode == "none",
      "SQLite3 storage does not support MCAP compression options.");
  options_ = options;
  options_.output_path = std::filesystem::absolute(options.output_path);
  if (std::filesystem::exists(options_.output_path)) {
    MW_ASSERT(
        std::filesystem::is_directory(options_.output_path) &&
            std::filesystem::is_empty(options_.output_path),
        "SQLite bag output '{}' must be an empty directory.",
        options_.output_path.string());
  } else {
    std::filesystem::create_directories(options_.output_path);
  }
  summary_.output_path = options_.output_path;
  open_ = true;
  OpenNewFile();
}

StorageSchemaId SqliteStorage::RegisterSchema(
    const ibmw::util::SchemaDescriptor& schema) {
  MW_ASSERT(open_, "SQLite storage is not open.");
  MW_ASSERT(
      !schema.type_name.empty() && !schema.serialization_type.empty() &&
          !schema.schema_name.empty() && !schema.schema_encoding.empty() &&
          !schema.schema_data.empty() &&
          schema.schema_hash ==
              ibmw::util::ComputeSchemaHash(schema.schema_data),
      "Invalid Schema '{}'.", schema.type_name);
  const std::string key = SchemaKey(schema);
  const auto existing = schema_ids_.find(key);
  if (existing != schema_ids_.end()) {
    const auto descriptor = schemas_.find(existing->second);
    MW_ASSERT(
        descriptor != schemas_.end() &&
            descriptor->second.schema_hash == schema.schema_hash &&
            descriptor->second.schema_name == schema.schema_name &&
            descriptor->second.schema_encoding == schema.schema_encoding,
        "Conflicting Schema for type '{}' serialization '{}'.",
        schema.type_name, schema.serialization_type);
    return existing->second;
  }
  const auto id = next_schema_id_++;
  schema_ids_.emplace(key, id);
  schemas_.emplace(id, schema);
  sqlite3_stmt* statement = nullptr;
  MW_ASSERT(
      sqlite3_prepare_v2(
          database_,
          "INSERT INTO schemas VALUES(?,?,?,?,?,?,?)", -1, &statement,
          nullptr) == SQLITE_OK,
      "Failed to prepare SQLite Schema insert.");
  sqlite3_bind_int(statement, 1, id);
  BindText(statement, 2, schema.type_name);
  BindText(statement, 3, schema.serialization_type);
  BindText(statement, 4, schema.schema_name);
  BindText(statement, 5, schema.schema_encoding);
  BindText(statement, 6, schema.schema_data);
  BindText(statement, 7, schema.schema_hash);
  const int result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  MW_ASSERT(result == SQLITE_DONE, "Failed to insert SQLite Schema.");
  return id;
}

StorageChannelId SqliteStorage::RegisterChannel(
    const StorageChannelDescriptor& channel,
    StorageSchemaId schema_id) {
  MW_ASSERT(open_, "SQLite storage is not open.");
  const auto schema = schemas_.find(schema_id);
  MW_ASSERT(schema != schemas_.end(), "Unknown SQLite Schema ID.");
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
  const auto existing = channel_ids_.find(key);
  if (existing != channel_ids_.end()) {
    const auto stored = channels_.find(existing->second);
    MW_ASSERT(
        stored != channels_.end() && stored->second.schema_id == schema_id,
        "Existing Channel '{}' references a different Schema.",
        channel.topic.topic);
    return existing->second;
  }
  const auto id = next_channel_id_++;
  channel_ids_.emplace(key, id);
  channels_.emplace(
      id, StoredChannel{.schema_id = schema_id, .descriptor = channel});
  sqlite3_stmt* statement = nullptr;
  MW_ASSERT(
      sqlite3_prepare_v2(
          database_,
          "INSERT INTO channels VALUES(?,?,?,?,?,?)", -1,
          &statement, nullptr) == SQLITE_OK,
      "Failed to prepare SQLite Channel insert.");
  sqlite3_bind_int(statement, 1, id);
  sqlite3_bind_int(statement, 2, schema_id);
  BindText(statement, 3, channel.topic.topic);
  BindText(statement, 4, channel.topic.msg_type);
  BindText(statement, 5, channel.topic.serialization_type);
  BindText(statement, 6, JsonQos(channel.topic.qos));
  const int result = sqlite3_step(statement);
  sqlite3_finalize(statement);
  MW_ASSERT(result == SQLITE_DONE, "Failed to insert SQLite Channel.");
  summary_.channel_message_counts.emplace(id, 0);
  return id;
}

void SqliteStorage::Write(
    StorageChannelId channel_id,
    const ibmw::transport::SerializedMessage& message) {
  MW_ASSERT(open_ && message.payload != nullptr, "SQLite storage is not writable.");
  const auto channel = channels_.find(channel_id);
  MW_ASSERT(channel != channels_.end(), "Unknown SQLite Channel ID.");
  MW_ASSERT(
      message.topic.topic == channel->second.descriptor.topic.topic &&
          message.topic.msg_type == channel->second.descriptor.topic.msg_type &&
          message.topic.serialization_type ==
              channel->second.descriptor.topic.serialization_type,
      "Message descriptor does not match Channel ID {}.", channel_id);
  MW_ASSERT(
      message.identity.sequence <= std::numeric_limits<uint32_t>::max(),
      "Message sequence {} exceeds the cross-format uint32 range.",
      message.identity.sequence);
  const uint64_t log_time =
      message.receive_timestamp != 0 ? message.receive_timestamp
                                     : message.publish_timestamp;
  const uint64_t publish_time =
      message.publish_timestamp != 0 ? message.publish_timestamp : log_time;
  const std::string payload = message.payload->JoinToString();
  if (ShouldSplit(log_time, payload.size())) {
    current_split_reason_ =
        options_.max_bag_size > 0 &&
                current_file_estimated_size_ + payload.size() + 64 >
                    options_.max_bag_size
            ? "size"
            : "duration";
    CloseCurrentFile();
    OpenNewFile();
  }

  sqlite3_reset(insert_message_);
  sqlite3_clear_bindings(insert_message_);
  sqlite3_bind_int(insert_message_, 1, channel_id);
  sqlite3_bind_int64(
      insert_message_, 2, static_cast<sqlite3_int64>(message.identity.sequence));
  sqlite3_bind_int64(
      insert_message_, 3, static_cast<sqlite3_int64>(log_time));
  sqlite3_bind_int64(
      insert_message_, 4, static_cast<sqlite3_int64>(publish_time));
  sqlite3_bind_blob64(
      insert_message_, 5, payload.data(), payload.size(), SQLITE_TRANSIENT);
  MW_ASSERT(
      sqlite3_step(insert_message_) == SQLITE_DONE,
      "Failed to insert SQLite message: {}.", sqlite3_errmsg(database_));

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
  current_file_estimated_size_ += payload.size() + 64;
}

void SqliteStorage::Close() {
  if (!open_) {
    return;
  }
  CloseCurrentFile();
  summary_.closed_cleanly = requested_clean_close_;
  WriteMetadata();
  open_ = false;
}

void SqliteStorage::Flush() {
  MW_ASSERT(open_ && database_ != nullptr, "SQLite storage is not open.");
  Execute("COMMIT");
  Execute("BEGIN IMMEDIATE TRANSACTION");
}

void SqliteStorage::SetMessageStatistics(
    uint64_t dropped_message_count,
    uint64_t duplicate_message_count) {
  summary_.dropped_message_count = dropped_message_count;
  summary_.duplicate_message_count = duplicate_message_count;
}

void SqliteStorage::SetCloseStatus(
    bool clean,
    std::string error_message) {
  requested_clean_close_ = clean;
  summary_.error_message = std::move(error_message);
}

void SqliteStorage::OpenNewFile() {
  database_path_ = options_.output_path /
                   ("data_" + std::to_string(file_index_++) + ".db3");
  MW_ASSERT(
      sqlite3_open(database_path_.c_str(), &database_) == SQLITE_OK,
      "Cannot open SQLite database '{}'.", database_path_.string());
  ApplyConfiguration();
  CreateSchema();
  Execute("BEGIN IMMEDIATE TRANSACTION");
  RestoreDescriptors();
  MW_ASSERT(
      sqlite3_prepare_v2(
          database_,
          "INSERT INTO messages(channel_id,sequence,log_time,publish_time,payload) "
          "VALUES(?,?,?,?,?)",
          -1, &insert_message_, nullptr) == SQLITE_OK,
      "Cannot prepare SQLite message insert.");
  current_file_start_timestamp_ = 0;
  current_file_end_timestamp_ = 0;
  current_file_message_count_ = 0;
  current_file_estimated_size_ = 0;
}

void SqliteStorage::CloseCurrentFile() {
  if (database_ == nullptr) {
    return;
  }
  if (insert_message_ != nullptr) {
    sqlite3_finalize(insert_message_);
    insert_message_ = nullptr;
  }
  Execute("COMMIT");
  sqlite3_close(database_);
  database_ = nullptr;
  const uint64_t size = std::filesystem::file_size(database_path_);
  summary_.file_size += size;
  summary_.relative_files.emplace_back(database_path_.filename());
  summary_.files.emplace_back(StorageFileSummary{
      .relative_path = database_path_.filename(),
      .start_timestamp = current_file_start_timestamp_,
      .end_timestamp = current_file_end_timestamp_,
      .message_count = current_file_message_count_,
      .file_size = size,
      .compression = "none",
      .split_reason = current_split_reason_});
  current_split_reason_.clear();
}

void SqliteStorage::CreateSchema() {
  Execute(
      "CREATE TABLE schemas(id INTEGER PRIMARY KEY,type_name TEXT,"
      "serialization_type TEXT,schema_name TEXT,schema_encoding TEXT,"
      "schema_data BLOB,schema_hash TEXT)");
  Execute(
      "CREATE TABLE channels(id INTEGER PRIMARY KEY,schema_id INTEGER,"
      "topic TEXT,msg_type TEXT,serialization_type TEXT,qos_json TEXT)");
  Execute(
      "CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "channel_id INTEGER,sequence INTEGER,log_time INTEGER,"
      "publish_time INTEGER,payload BLOB)");
  Execute("CREATE INDEX messages_timestamp_idx ON messages(log_time,id)");
}

void SqliteStorage::RestoreDescriptors() {
  for (const auto& [id, schema] : schemas_) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        database_, "INSERT INTO schemas VALUES(?,?,?,?,?,?,?)", -1,
        &statement, nullptr);
    sqlite3_bind_int(statement, 1, id);
    BindText(statement, 2, schema.type_name);
    BindText(statement, 3, schema.serialization_type);
    BindText(statement, 4, schema.schema_name);
    BindText(statement, 5, schema.schema_encoding);
    BindText(statement, 6, schema.schema_data);
    BindText(statement, 7, schema.schema_hash);
    MW_ASSERT(sqlite3_step(statement) == SQLITE_DONE, "Cannot restore Schema.");
    sqlite3_finalize(statement);
  }
  for (const auto& [id, channel] : channels_) {
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(
        database_, "INSERT INTO channels VALUES(?,?,?,?,?,?)", -1,
        &statement, nullptr);
    sqlite3_bind_int(statement, 1, id);
    sqlite3_bind_int(statement, 2, channel.schema_id);
    BindText(statement, 3, channel.descriptor.topic.topic);
    BindText(statement, 4, channel.descriptor.topic.msg_type);
    BindText(statement, 5, channel.descriptor.topic.serialization_type);
    BindText(statement, 6, JsonQos(channel.descriptor.topic.qos));
    MW_ASSERT(sqlite3_step(statement) == SQLITE_DONE, "Cannot restore Channel.");
    sqlite3_finalize(statement);
  }
}

void SqliteStorage::ApplyConfiguration() {
  if (options_.storage_preset_profile == "resilient") {
    Execute("PRAGMA journal_mode=WAL");
    Execute("PRAGMA synchronous=FULL");
  } else if (options_.storage_preset_profile == "performance") {
    Execute("PRAGMA journal_mode=MEMORY");
    Execute("PRAGMA synchronous=OFF");
  }
  if (!options_.storage_config_file.empty()) {
    const auto root = YAML::LoadFile(options_.storage_config_file.string());
    const auto pragmas = root["pragmas"];
    MW_ASSERT(
        !pragmas || pragmas.IsMap(),
        "SQLite storage config 'pragmas' must be a map.");
    if (pragmas) {
      for (const auto& pragma : pragmas) {
        Execute(
            "PRAGMA " + pragma.first.as<std::string>() + "=" +
            pragma.second.as<std::string>());
      }
    }
  }
}

bool SqliteStorage::ShouldSplit(
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

void SqliteStorage::Execute(std::string_view sql) const {
  char* error = nullptr;
  const int result = sqlite3_exec(
      database_, std::string(sql).c_str(), nullptr, nullptr, &error);
  const std::string message = error == nullptr ? "" : error;
  sqlite3_free(error);
  MW_ASSERT(result == SQLITE_OK, "SQLite command failed: {}.", message);
}

void SqliteStorage::WriteMetadata() {
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
  information["compression_mode"] = "none";
  information["compression_format"] = "";

  YAML::Node files(YAML::NodeType::Sequence);
  for (const auto& summary_file : summary_.files) {
    YAML::Node file;
    file["path"] = summary_file.relative_path.string();
    file["start_timestamp"] = summary_file.start_timestamp;
    file["end_timestamp"] = summary_file.end_timestamp;
    file["message_count"] = summary_file.message_count;
    file["size"] = summary_file.file_size;
    file["compression"] = "none";
    file["split_reason"] = summary_file.split_reason;
    files.push_back(file);
  }
  information["files"] = files;

  YAML::Node schemas(YAML::NodeType::Sequence);
  for (const auto& [id, schema] : schemas_) {
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
  for (const auto& [id, channel] : channels_) {
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
  MW_ASSERT(output.is_open(), "Cannot write SQLite metadata.yaml.");
  output << root;
  output.close();
  MW_ASSERT(output.good(), "Failed to write SQLite metadata.yaml.");
}

std::string SqliteStorage::SchemaKey(
    const ibmw::util::SchemaDescriptor& schema) {
  // A logical type/serialization pair may have only one Schema in a Bag.
  // Keep the hash out of the key so a conflicting definition is rejected by
  // the descriptor check instead of being silently stored as a second Schema.
  return schema.type_name + '\0' + schema.serialization_type;
}

std::string SqliteStorage::ChannelKey(
    const StorageChannelDescriptor& channel) {
  return channel.topic.topic + '\0' + channel.topic.msg_type + '\0' +
         channel.topic.serialization_type + '\0' +
         channel.topic.schema.schema_hash;
}

}  // namespace ibmw::extensions::record_playback_extension
