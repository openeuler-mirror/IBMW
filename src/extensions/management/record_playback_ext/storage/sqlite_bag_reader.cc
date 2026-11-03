// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/sqlite_bag_reader.h"
#include "record_playback_ext/storage/bag_reader_util.h"
#include "record_playback_ext/storage/path_safety.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <queue>
#include <set>

#include <sqlite3.h>

#include "base/exception.h"
#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

std::string ColumnText(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  const int size = sqlite3_column_bytes(statement, column);
  return value == nullptr
             ? std::string{}
             : std::string(reinterpret_cast<const char*>(value), size);
}

uint16_t ReadId(sqlite3_stmt* statement, int column, std::string_view what) {
  MW_ASSERT(sqlite3_column_type(statement, column) == SQLITE_INTEGER,
            "SQLite {} ID must be an integer.", what);
  const auto value = sqlite3_column_int64(statement, column);
  MW_ASSERT(value >= 0 &&
                static_cast<uint64_t>(value) <=
                    std::numeric_limits<uint16_t>::max(),
            "SQLite {} ID {} is outside uint16 range.", what, value);
  return static_cast<uint16_t>(value);
}

std::shared_ptr<ibmw::util::BufferArrayView> CopyBlob(
    sqlite3_stmt* statement,
    int column) {
  const auto* data = static_cast<const uint8_t*>(
      sqlite3_column_blob(statement, column));
  const size_t size = static_cast<size_t>(
      sqlite3_column_bytes(statement, column));
  return MakeOwnedPayload(data, size);
}

int64_t ReadInteger(const YAML::Node& node, std::string_view name) {
  MW_ASSERT(
      node.IsScalar(), "SQLite reader config '{}' must be an integer.", name);
  try {
    return node.as<int64_t>();
  } catch (const YAML::Exception&) {
    MW_ASSERT(
        false, "SQLite reader config '{}' must be an integer.", name);
  }
  return 0;
}

void LoadSqliteReaderConfiguration(
    const std::filesystem::path& path,
    int& busy_timeout_ms,
    std::vector<std::pair<std::string, int64_t>>& pragmas,
    bool& integrity_check) {
  busy_timeout_ms = 0;
  pragmas.clear();
  integrity_check = true;
  if (path.empty()) {
    return;
  }
  const auto root = YAML::LoadFile(path.string());
  MW_ASSERT(root && root.IsMap(), "SQLite reader config must be a map.");
  for (const auto& entry : root) {
    MW_ASSERT(
        entry.first.IsScalar(), "SQLite reader config keys must be strings.");
    const std::string key = entry.first.as<std::string>();
    MW_ASSERT(
        key == "busy_timeout_ms" || key == "pragmas" || key == "integrity_check",
        "Unknown SQLite reader config key '{}'. Supported keys are "
        "busy_timeout_ms, pragmas and integrity_check.",
        key);
    if (key == "integrity_check") {
      // PRAGMA quick_check reads the whole file; large trusted bags may skip
      // it (EXT-036).
      MW_ASSERT(entry.second.IsScalar(), "SQLite reader config 'integrity_check' must be a boolean.");
      integrity_check = entry.second.as<bool>();
      continue;
    }
    if (key == "busy_timeout_ms") {
      const int64_t value = ReadInteger(entry.second, key);
      MW_ASSERT(
          value >= 0 &&
              value <= static_cast<int64_t>(std::numeric_limits<int>::max()),
          "SQLite busy_timeout_ms is outside the supported range.");
      busy_timeout_ms = static_cast<int>(value);
      continue;
    }

    MW_ASSERT(
        entry.second.IsMap(), "SQLite reader config 'pragmas' must be a map.");
    for (const auto& pragma : entry.second) {
      MW_ASSERT(
          pragma.first.IsScalar(), "SQLite PRAGMA names must be strings.");
      const std::string name = pragma.first.as<std::string>();
      MW_ASSERT(
          name == "cache_size" || name == "mmap_size",
          "Unsupported SQLite reader PRAGMA '{}'. Supported PRAGMAs are "
          "cache_size and mmap_size.",
          name);
      const int64_t value = ReadInteger(pragma.second, "pragmas." + name);
      MW_ASSERT(
          name != "mmap_size" || value >= 0,
          "SQLite reader PRAGMA mmap_size cannot be negative.");
      pragmas.emplace_back(name, value);
    }
  }
}

void ApplySqliteReaderConfiguration(
    sqlite3* database,
    int busy_timeout_ms,
    const std::vector<std::pair<std::string, int64_t>>& pragmas) {
  MW_ASSERT(
      sqlite3_busy_timeout(database, busy_timeout_ms) == SQLITE_OK,
      "Cannot apply SQLite reader busy_timeout_ms.");
  for (const auto& [name, value] : pragmas) {
    char* error = nullptr;
    const std::string sql =
        "PRAGMA " + name + "=" + std::to_string(value);
    const int result = sqlite3_exec(
        database, sql.c_str(), nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "" : error;
    sqlite3_free(error);
    MW_ASSERT(
        result == SQLITE_OK, "Cannot apply SQLite reader PRAGMA '{}': {}.",
        name, message);
  }
}

// Finalizes a statement on scope exit, also when a check throws: an
// unfinalized statement makes sqlite3_close fail and leak the connection.
struct StatementGuard {
  sqlite3_stmt** statement;
  ~StatementGuard() { sqlite3_finalize(*statement); }
};

bool SqliteIntegrityOk(sqlite3* database) {
  sqlite3_stmt* statement = nullptr;
  StatementGuard statement_guard{&statement};
  if (sqlite3_prepare_v2(
          database, "PRAGMA quick_check", -1, &statement, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  bool saw_result = false;
  bool valid = true;
  int result = SQLITE_ROW;
  while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
    saw_result = true;
    valid = valid && ColumnText(statement, 0) == "ok";  // may throw bad_alloc
  }
  return saw_result && valid && result == SQLITE_DONE;
}

// qos_json comes from the bag file: reject anything but a flat object of
// strings with an MW exception (EXT-036).
std::map<std::string, std::string> ParseQos(const std::string& text, uint16_t channel_id) {
  const auto json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  MW_ASSERT(json.is_object(), "SQLite channel {} has invalid qos_json.", channel_id);
  std::map<std::string, std::string> policies;
  for (const auto& [name, value] : json.items()) {
    MW_ASSERT(value.is_string(), "SQLite channel {} has a non-string qos policy '{}'.", channel_id, name);
    policies.emplace(name, value.get<std::string>());
  }
  return policies;
}

}  // namespace

void SqliteBagReader::Open(
    const std::filesystem::path& bag_path,
    const ReaderOpenOptions& options) {
  info_ = {};
  file_channels_.clear();
  LoadSqliteReaderConfiguration(
      options.storage_config_file, busy_timeout_ms_, pragmas_, integrity_check_);
  const auto absolute_path = std::filesystem::absolute(bag_path);
  const bool direct_file = std::filesystem::is_regular_file(absolute_path);
  MW_ASSERT(
      std::filesystem::is_directory(absolute_path) ||
          (direct_file && absolute_path.extension() == ".db3"),
      "SQLite bag path '{}' is invalid.", absolute_path.string());
  info_.bag_path = std::filesystem::canonical(absolute_path);
  YAML::Node metadata;
  if (direct_file) {
    info_.version = 1;
    info_.storage_id = "sqlite3";
    info_.closed_cleanly = false;
  } else {
    const auto metadata_path = info_.bag_path / "metadata.yaml";
    MW_ASSERT(
        std::filesystem::is_regular_file(metadata_path),
        "SQLite bag metadata does not exist.");
    const auto root = YAML::LoadFile(metadata_path.string());
    metadata = root["ibmw_bagfile_information"];
    MW_ASSERT(metadata && metadata.IsMap(), "Invalid SQLite bag metadata.");
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
  }
  MW_ASSERT(
      info_.storage_id == "sqlite3" &&
          (options.expected_storage_id.empty() ||
           options.expected_storage_id == "sqlite3"),
      "Requested Storage Plugin does not match SQLite3.");

  std::vector<std::filesystem::path> files;
  std::vector<YAML::Node> file_nodes;
  if (direct_file) {
    files.emplace_back(std::filesystem::canonical(absolute_path));
    file_nodes.emplace_back();
  } else {
    for (const auto& file : metadata["files"]) {
      const auto path = SafeBagFilePath(
          info_.bag_path, file["path"].as<std::string>());
      MW_ASSERT(std::filesystem::is_regular_file(path), "Missing SQLite bag file.");
      files.emplace_back(std::filesystem::canonical(path));
      file_nodes.emplace_back(file);
    }
  }

  std::unordered_map<std::string, size_t> schema_indices;
  std::unordered_map<std::string, size_t> channel_indices;
  for (size_t file_index = 0; file_index < files.size(); ++file_index) {
    // sqlite3_open_v2 returns a connection also when it fails: closed by the
    // guard. close_v2 frees it even if a statement is still unfinalized.
    sqlite3* database = nullptr;
    struct DatabaseGuard {
      sqlite3** database;
      ~DatabaseGuard() { sqlite3_close_v2(*database); }
    } guard{&database};
    MW_ASSERT(
        sqlite3_open_v2(
            files[file_index].c_str(), &database, SQLITE_OPEN_READONLY,
            nullptr) == SQLITE_OK,
        "Cannot open SQLite bag file.");
    ApplySqliteReaderConfiguration(database, busy_timeout_ms_, pragmas_);
    if (integrity_check_) {
      MW_ASSERT(
          SqliteIntegrityOk(database), "SQLite integrity check failed for '{}'.",
          files[file_index].string());
      // A single file has no metadata: it counts as cleanly closed when it
      // passes the check, and stays unverified when the check is skipped.
      if (direct_file) info_.closed_cleanly = true;
    }

    BagFileInfo file{
        .path = files[file_index],
        .size = std::filesystem::file_size(files[file_index]),
        .compression = "none",
        .split_reason =
            file_nodes[file_index]
                ? file_nodes[file_index]["split_reason"].as<std::string>("")
                : ""};
    sqlite3_stmt* statistics = nullptr;
    StatementGuard statistics_guard{&statistics};
    MW_ASSERT(sqlite3_prepare_v2(
        database,
        "SELECT COUNT(*),COALESCE(MIN(log_time),0),COALESCE(MAX(log_time),0) "
        "FROM messages",
        -1, &statistics, nullptr) == SQLITE_OK && statistics != nullptr,
        "Cannot prepare SQLite statistics query: {}.", sqlite3_errmsg(database));
    MW_ASSERT(sqlite3_step(statistics) == SQLITE_ROW, "Cannot read SQLite statistics.");
    file.message_count = static_cast<uint64_t>(sqlite3_column_int64(statistics, 0));
    file.start_timestamp = static_cast<uint64_t>(sqlite3_column_int64(statistics, 1));
    file.end_timestamp = static_cast<uint64_t>(sqlite3_column_int64(statistics, 2));
    info_.closed_cleanly =
        info_.closed_cleanly &&
        MetadataFileMatches(file_nodes[file_index], file);
    const bool had_messages = info_.message_count != 0;
    info_.files.emplace_back(file);
    info_.message_count += file.message_count;
    info_.total_size += file.size;
    if (file.message_count != 0) {
      if (!had_messages) {
        info_.start_timestamp = file.start_timestamp;
        info_.end_timestamp = file.end_timestamp;
      } else {
        info_.start_timestamp = std::min(info_.start_timestamp, file.start_timestamp);
        info_.end_timestamp = std::max(info_.end_timestamp, file.end_timestamp);
      }
    }

    std::unordered_map<uint16_t, BagSchemaInfo> schemas;
    sqlite3_stmt* schema_query = nullptr;
    StatementGuard schema_query_guard{&schema_query};
    MW_ASSERT(sqlite3_prepare_v2(
        database,
        "SELECT id,type_name,serialization_type,schema_name,schema_encoding,"
        "schema_data,schema_hash FROM schemas ORDER BY id",
        -1, &schema_query, nullptr) == SQLITE_OK && schema_query != nullptr,
        "Cannot prepare SQLite schemas query: {}.", sqlite3_errmsg(database));
    int schema_result = SQLITE_ROW;
    while ((schema_result = sqlite3_step(schema_query)) == SQLITE_ROW) {
      BagSchemaInfo schema{
          .id = ReadId(schema_query, 0, "schema"),
          .descriptor =
              {.type_name = ColumnText(schema_query, 1),
               .serialization_type = ColumnText(schema_query, 2),
               .schema_name = ColumnText(schema_query, 3),
               .schema_encoding = ColumnText(schema_query, 4),
               .schema_data = ColumnText(schema_query, 5),
               .schema_hash = ColumnText(schema_query, 6)}};
      schema.hash_valid =
          schema.descriptor.schema_hash ==
          ibmw::util::ComputeSchemaHash(schema.descriptor.schema_data);
      MW_ASSERT(
          schemas.emplace(schema.id, schema).second,
          "SQLite file contains duplicate schema ID {}.", schema.id);
      const std::string key =
          schema.descriptor.type_name + '\0' +
          schema.descriptor.serialization_type + '\0' +
          schema.descriptor.schema_hash;
      if (!schema_indices.contains(key)) {
        schema_indices.emplace(key, info_.schemas.size());
        info_.schemas.emplace_back(schema);
      }
    }
    MW_ASSERT(schema_result == SQLITE_DONE,
              "Cannot read SQLite schemas: {}.", sqlite3_errmsg(database));

    // messages has no index on channel_id, so each COUNT(*) is a full scan.
    // With few channels one scan per channel is cheapest; beyond that one
    // GROUP BY pass wins despite its sort (measured crossover ~4, EXT-036).
    constexpr int64_t kPerChannelCountLimit = 3;
    sqlite3_stmt* count_query = nullptr;
    StatementGuard count_query_guard{&count_query};
    const auto prepare_count = [&](const char* sql) {
      MW_ASSERT(sqlite3_prepare_v2(database, sql, -1, &count_query, nullptr) == SQLITE_OK &&
                    count_query != nullptr,
                "Cannot prepare SQLite channel count query: {}.", sqlite3_errmsg(database));
    };
    prepare_count("SELECT COUNT(*) FROM channels");
    MW_ASSERT(sqlite3_step(count_query) == SQLITE_ROW,
              "Cannot count SQLite channels: {}.", sqlite3_errmsg(database));
    const bool count_per_channel = sqlite3_column_int64(count_query, 0) <= kPerChannelCountLimit;
    sqlite3_finalize(count_query);
    count_query = nullptr;

    std::unordered_map<int64_t, uint64_t> channel_message_counts;
    if (count_per_channel) {
      prepare_count("SELECT COUNT(*) FROM messages WHERE channel_id=?");
    } else {
      prepare_count("SELECT channel_id,COUNT(*) FROM messages GROUP BY channel_id");
      int count_result = SQLITE_ROW;
      while ((count_result = sqlite3_step(count_query)) == SQLITE_ROW) {
        if (sqlite3_column_type(count_query, 0) != SQLITE_INTEGER) continue;
        channel_message_counts[sqlite3_column_int64(count_query, 0)] =
            static_cast<uint64_t>(sqlite3_column_int64(count_query, 1));
      }
      MW_ASSERT(count_result == SQLITE_DONE,
                "Cannot count SQLite channel messages: {}.", sqlite3_errmsg(database));
    }
    const auto message_count = [&](int64_t channel_id) -> uint64_t {
      if (!count_per_channel) {
        const auto count = channel_message_counts.find(channel_id);
        return count == channel_message_counts.end() ? 0 : count->second;
      }
      sqlite3_reset(count_query);
      sqlite3_bind_int64(count_query, 1, channel_id);
      MW_ASSERT(sqlite3_step(count_query) == SQLITE_ROW,
                "Cannot count SQLite channel messages: {}.", sqlite3_errmsg(database));
      return static_cast<uint64_t>(sqlite3_column_int64(count_query, 0));
    };

    sqlite3_stmt* channel_query = nullptr;
    StatementGuard channel_query_guard{&channel_query};
    MW_ASSERT(sqlite3_prepare_v2(
        database,
        "SELECT c.id,c.schema_id,c.topic,c.msg_type,c.serialization_type,"
        "c.qos_json FROM channels c ORDER BY c.id",
        -1, &channel_query, nullptr) == SQLITE_OK && channel_query != nullptr,
        "Cannot prepare SQLite channels query: {}.", sqlite3_errmsg(database));
    int channel_result = SQLITE_ROW;
    while ((channel_result = sqlite3_step(channel_query)) == SQLITE_ROW) {
      const auto schema_id = ReadId(channel_query, 1, "schema");
      MW_ASSERT(schemas.contains(schema_id), "SQLite Channel has no Schema.");
      BagChannelInfo channel{
          .id = ReadId(channel_query, 0, "channel"),
          .schema_id = schema_id,
          .topic =
              {.topic = ColumnText(channel_query, 2),
               .msg_type = ColumnText(channel_query, 3),
               .serialization_type = ColumnText(channel_query, 4),
               .schema = schemas.at(schema_id).descriptor},
          .message_count = 0};
      channel.message_count = message_count(channel.id);
      channel.topic.qos.policies = ParseQos(ColumnText(channel_query, 5), channel.id);
      MW_ASSERT(
          file_channels_.emplace(
          FileChannelKey{
              .file_index = file_index, .channel_id = channel.id},
          channel)
              .second,
          "SQLite file contains duplicate channel ID {}.", channel.id);
      const std::string key =
          channel.topic.topic + '\0' + channel.topic.msg_type + '\0' +
          channel.topic.serialization_type + '\0' +
          channel.topic.schema.schema_hash;
      const auto existing = channel_indices.find(key);
      if (existing == channel_indices.end()) {
        channel_indices.emplace(key, info_.channels.size());
        info_.channels.emplace_back(std::move(channel));
      } else {
        info_.channels[existing->second].message_count +=
            channel.message_count;
      }
    }
    MW_ASSERT(channel_result == SQLITE_DONE,
              "Cannot read SQLite channels: {}.", sqlite3_errmsg(database));
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

void SqliteBagReader::ReadMessages(
    const BagReadOptions& options,
    const MessageCallback& callback) const {
  MW_ASSERT(callback, "SQLite message callback cannot be empty.");
  const std::set<std::string> topics(options.topics.begin(), options.topics.end());

  struct FileCursor {
    size_t file_index = 0;
    sqlite3* database = nullptr;
    sqlite3_stmt* query = nullptr;
    std::optional<BagMessage> current;

    ~FileCursor() {
      sqlite3_finalize(query);
      sqlite3_close_v2(database);
    }
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

  const auto advance = [&](FileCursor& cursor) {
    while (true) {
      const int result = sqlite3_step(cursor.query);
      if (result == SQLITE_DONE) {
        cursor.current.reset();
        return false;
      }
      MW_ASSERT(
          result == SQLITE_ROW,
          "Cannot read SQLite messages from '{}': {}.",
          info_.files[cursor.file_index].path.string(),
          sqlite3_errmsg(cursor.database));
      const auto channel_id = ReadId(cursor.query, 0, "channel");
      const auto sequence_value = sqlite3_column_int64(cursor.query, 1);
      MW_ASSERT(sqlite3_column_type(cursor.query, 1) == SQLITE_INTEGER &&
                    sequence_value >= 0 &&
                    static_cast<uint64_t>(sequence_value) <=
                        std::numeric_limits<uint32_t>::max(),
                "SQLite message sequence is outside uint32 range.");
      const auto channel = file_channels_.find(FileChannelKey{
          .file_index = cursor.file_index, .channel_id = channel_id});
      MW_ASSERT(channel != file_channels_.end(), "Unknown SQLite Channel.");
      if (!topics.empty() && !topics.contains(channel->second.topic.topic)) {
        continue;
      }
      cursor.current.emplace(BagMessage{
          .channel = channel->second,
          .log_time = static_cast<uint64_t>(
              sqlite3_column_int64(cursor.query, 2)),
          .publish_time = static_cast<uint64_t>(
              sqlite3_column_int64(cursor.query, 3)),
          .sequence = static_cast<uint32_t>(sequence_value),
          .payload = CopyBlob(cursor.query, 4)});
      return true;
    }
  };

  for (size_t file_index = 0; file_index < info_.files.size(); ++file_index) {
    auto cursor = std::make_unique<FileCursor>();
    cursor->file_index = file_index;
    MW_ASSERT(
        sqlite3_open_v2(
            info_.files[file_index].path.c_str(), &cursor->database,
            SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "Cannot open SQLite bag for playback.");
    ApplySqliteReaderConfiguration(
        cursor->database, busy_timeout_ms_, pragmas_);
    std::string sql =
        "SELECT channel_id,sequence,log_time,publish_time,payload FROM messages "
        "WHERE log_time>=?";
    if (options.end_timestamp != 0) {
      sql += " AND log_time<?";
    }
    sql += " ORDER BY log_time,id";
    MW_ASSERT(
        sqlite3_prepare_v2(
            cursor->database, sql.c_str(), -1, &cursor->query, nullptr) ==
            SQLITE_OK,
        "Cannot prepare SQLite playback query for '{}': {}.",
        info_.files[file_index].path.string(),
        sqlite3_errmsg(cursor->database));
    MW_ASSERT(
        sqlite3_bind_int64(
            cursor->query, 1,
            static_cast<sqlite3_int64>(options.start_timestamp)) == SQLITE_OK,
        "Cannot bind SQLite playback start timestamp.");
    if (options.end_timestamp != 0) {
      MW_ASSERT(
          sqlite3_bind_int64(
              cursor->query, 2,
              static_cast<sqlite3_int64>(options.end_timestamp)) == SQLITE_OK,
          "Cannot bind SQLite playback end timestamp.");
    }
    const size_t cursor_index = cursors.size();
    if (advance(*cursor)) {
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
    MW_ASSERT(cursor.current, "SQLite file cursor has no pending message.");
    auto message = std::move(*cursor.current);
    cursor.current.reset();
    if (!callback(std::move(message))) {
      return;
    }
    if (advance(cursor)) {
      pending.push(PendingMessage{
          .log_time = cursor.current->log_time,
          .file_index = cursor.file_index,
          .cursor_index = next.cursor_index});
    }
  }
}

}  // namespace ibmw::extensions::record_playback_extension
