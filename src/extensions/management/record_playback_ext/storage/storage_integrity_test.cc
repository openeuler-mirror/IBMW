// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "base/exception.h"
#include "record_playback_ext/storage/mcap_bag_reader.h"
#include "record_playback_ext/storage/mcap_storage.h"
#include "record_playback_ext/storage/path_safety.h"
#include "record_playback_ext/storage/sqlite_bag_reader.h"
#include "record_playback_ext/storage/sqlite_storage.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view name) {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            (std::string(name) + "-" + std::to_string(::getpid()) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void ExecuteSql(
    const std::filesystem::path& path,
    const std::vector<std::string>& statements) {
  sqlite3* database = nullptr;
  ASSERT_EQ(sqlite3_open(path.c_str(), &database), SQLITE_OK);
  for (const auto& statement : statements) {
    char* error = nullptr;
    const int result =
        sqlite3_exec(database, statement.c_str(), nullptr, nullptr, &error);
    const std::string message = error == nullptr ? "" : error;
    sqlite3_free(error);
    ASSERT_EQ(result, SQLITE_OK) << message << ": " << statement;
  }
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
}

const std::string kSchemasTable =
    "CREATE TABLE schemas(id INTEGER,type_name TEXT,serialization_type TEXT,"
    "schema_name TEXT,schema_encoding TEXT,schema_data BLOB,schema_hash TEXT)";
const std::string kChannelsTable =
    "CREATE TABLE channels(id INTEGER,schema_id INTEGER,topic TEXT,msg_type "
    "TEXT,serialization_type TEXT,qos_json TEXT)";
const std::string kMessagesTable =
    "CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,channel_id "
    "INTEGER,sequence INTEGER,log_time INTEGER,publish_time INTEGER,payload "
    "BLOB)";

ibmw::util::SchemaDescriptor TestSchema() {
  ibmw::util::SchemaDescriptor schema{
      .type_name = "test:StorageMessage",
      .serialization_type = "cdr",
      .schema_name = "StorageMessage",
      .schema_encoding = "omgidl",
      .schema_data = "struct StorageMessage { sequence<octet> data; };"};
  schema.schema_hash = ibmw::util::ComputeSchemaHash(schema.schema_data);
  return schema;
}

std::string InsertSchema(uint64_t id) {
  const auto schema = TestSchema();
  return "INSERT INTO schemas VALUES(" + std::to_string(id) + ",'" +
         schema.type_name + "','" + schema.serialization_type + "','" +
         schema.schema_name + "','" + schema.schema_encoding + "','" +
         schema.schema_data + "','" + schema.schema_hash + "')";
}

std::string InsertChannel(uint64_t id, uint64_t schema_id) {
  return "INSERT INTO channels VALUES(" + std::to_string(id) + "," +
         std::to_string(schema_id) +
         ",'/storage_test','test:StorageMessage','cdr','{}')";
}

std::string InsertMessage(uint64_t sequence) {
  return "INSERT INTO messages(channel_id,sequence,log_time,publish_time,"
         "payload) VALUES(1," +
         std::to_string(sequence) + ",101,100,X'01020304')";
}

std::shared_ptr<ibmw::util::BufferArrayView> OwnedPayload(
    std::string value) {
  auto storage = std::make_shared<std::string>(std::move(value));
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(storage->data(), storage->size()),
      [storage](const ibmw::util::BufferArrayView* view) { delete view; });
}

ibmw::transport::SerializedMessage TestMessage(
    uint64_t sequence) {
  auto schema = TestSchema();
  return ibmw::transport::SerializedMessage{
      .topic =
          {.topic = "/storage_test",
           .msg_type = schema.type_name,
           .serialization_type = schema.serialization_type,
           .schema = std::move(schema)},
      .identity =
          {.source_id = "storage-test",
           .publisher_id = "publisher",
           .sequence = sequence},
      .publish_timestamp = 100,
      .receive_timestamp = 101,
      .source_driver = "test",
      .payload = OwnedPayload("\x01\x02\x03\x04")};
}

template <typename StorageType, typename ReaderType>
void ExpectContentRoundTrip(
    const std::filesystem::path& path,
    std::string_view storage_id) {
  StorageType storage;
  storage.Open({.output_path = path, .storage_id = std::string(storage_id)});
  const auto schema_id = storage.RegisterSchema(TestSchema());
  const auto channel_id = storage.RegisterChannel(
      {.topic = TestMessage(0).topic}, schema_id);
  storage.Write(channel_id, TestMessage(std::numeric_limits<uint32_t>::max()));
  EXPECT_THROW(
      storage.Write(
          channel_id,
          TestMessage(
              static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) +
              1)),
      ibmw::common::util::IbmwException);
  storage.SetCloseStatus(true, {});
  storage.Close();

  ReaderType reader;
  ASSERT_NO_THROW(reader.Open(path));
  ASSERT_EQ(reader.Info().message_count, 1U);
  size_t count = 0;
  reader.ReadMessages({}, [&](BagMessage&& message) {
    ++count;
    EXPECT_EQ(message.channel.topic.topic, "/storage_test");
    EXPECT_EQ(message.sequence, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(message.publish_time, 100U);
    EXPECT_EQ(message.log_time, 101U);
    EXPECT_NE(message.payload, nullptr);
    if (message.payload != nullptr) {
      EXPECT_EQ(message.payload->JoinToString(), "\x01\x02\x03\x04");
    }
    return true;
  });
  EXPECT_EQ(count, 1U);
}

TEST(BagPathSafetyTest, RejectsAbsoluteParentAndSymlinkEscapes) {
  TemporaryDirectory temporary("ibmw-bag-path-safety");
  const auto bag = temporary.Path() / "bag";
  const auto outside = temporary.Path() / "outside.mcap";
  std::filesystem::create_directories(bag / "nested");
  std::ofstream(bag / "nested" / "data.mcap") << "inside";
  std::ofstream(outside) << "outside";
  std::filesystem::create_symlink(outside, bag / "escape.mcap");

  EXPECT_EQ(
      SafeBagFilePath(bag, "nested/data.mcap"),
      std::filesystem::canonical(bag / "nested" / "data.mcap"));
  EXPECT_THROW(
      SafeBagFilePath(bag, outside), ibmw::common::util::IbmwException);
  EXPECT_THROW(
      SafeBagFilePath(bag, "../outside.mcap"),
      ibmw::common::util::IbmwException);
  EXPECT_THROW(
      SafeBagFilePath(bag, "escape.mcap"),
      ibmw::common::util::IbmwException);
}

size_t OpenFileDescriptorCount() {
  return static_cast<size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"),
      std::filesystem::directory_iterator()));
}

TEST(SqliteBagReaderIntegrityTest, RejectsCorruptAndMissingTables) {
  TemporaryDirectory temporary("ibmw-sqlite-corrupt");
  const auto corrupt = temporary.Path() / "corrupt.db3";
  std::ofstream(corrupt, std::ios::binary) << "not a sqlite database";
  SqliteBagReader reader;
  EXPECT_THROW(reader.Open(corrupt), ibmw::common::util::IbmwException);

  const std::vector<std::vector<std::string>> missing_table_fixtures{
      {kSchemasTable, kChannelsTable},
      {kSchemasTable, kMessagesTable},
      {kChannelsTable, kMessagesTable}};
  for (size_t index = 0; index < missing_table_fixtures.size(); ++index) {
    const auto path =
        temporary.Path() / ("missing-" + std::to_string(index) + ".db3");
    ExecuteSql(path, missing_table_fixtures[index]);
    SqliteBagReader missing_table_reader;
    EXPECT_THROW(
        missing_table_reader.Open(path), ibmw::common::util::IbmwException)
        << "fixture index " << index;
  }
}

// EXT-035: a bag file SQLite can not open (no read permission, or corrupt
// when running as root) leaves no connection behind.
TEST(SqliteBagReaderIntegrityTest, FailedOpenDoesNotLeakConnection) {
  TemporaryDirectory temporary("ibmw-sqlite-unreadable");
  const auto path = temporary.Path() / "unreadable.db3";
  std::ofstream(path, std::ios::binary) << "not a sqlite database";
  std::filesystem::permissions(path, std::filesystem::perms::none);
  const auto open = [&path] {
    SqliteBagReader reader;
    EXPECT_THROW(reader.Open(path), ibmw::common::util::IbmwException);
  };
  open();  // SQLite allocates its globals once
  const auto before = sqlite3_memory_used();
  for (int i = 0; i < 10; ++i) open();
  EXPECT_EQ(sqlite3_memory_used(), before);
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
}

TEST(SqliteBagReaderIntegrityTest, RejectsInvalidReferencesAndNarrowing) {
  TemporaryDirectory temporary("ibmw-sqlite-ids");
  const auto expect_rejected = [&](std::string_view name,
                                   std::vector<std::string> rows) {
    const auto path = temporary.Path() / (std::string(name) + ".db3");
    std::vector<std::string> statements{
        kSchemasTable, kChannelsTable, kMessagesTable};
    statements.insert(statements.end(), rows.begin(), rows.end());
    ExecuteSql(path, statements);
    const size_t open_fds = OpenFileDescriptorCount();
    {
      SqliteBagReader reader;
      EXPECT_THROW(reader.Open(path), ibmw::common::util::IbmwException)
          << name;
    }
    // A rejected file is closed again (no statement keeps it open).
    EXPECT_EQ(OpenFileDescriptorCount(), open_fds) << name;
  };

  expect_rejected("schema-overflow", {InsertSchema(65536)});
  expect_rejected(
      "channel-overflow", {InsertSchema(1), InsertChannel(65536, 1)});
  expect_rejected(
      "missing-schema", {InsertSchema(1), InsertChannel(1, 2)});
  expect_rejected(
      "duplicate-schema", {InsertSchema(1), InsertSchema(1)});
  expect_rejected(
      "duplicate-channel",
      {InsertSchema(1), InsertChannel(1, 1), InsertChannel(1, 1)});
}

TEST(SqliteBagReaderIntegrityTest, EnforcesSequenceBoundaryOnRead) {
  TemporaryDirectory temporary("ibmw-sqlite-sequence");
  const auto valid = temporary.Path() / "valid.db3";
  ExecuteSql(
      valid,
      {kSchemasTable, kChannelsTable, kMessagesTable, InsertSchema(1),
       InsertChannel(1, 1),
       InsertMessage(std::numeric_limits<uint32_t>::max())});
  SqliteBagReader valid_reader;
  ASSERT_NO_THROW(valid_reader.Open(valid));
  ASSERT_NO_THROW(valid_reader.ReadMessages({}, [](BagMessage&& message) {
    EXPECT_EQ(message.sequence, std::numeric_limits<uint32_t>::max());
    return true;
  }));

  const auto overflow = temporary.Path() / "overflow.db3";
  ExecuteSql(
      overflow,
      {kSchemasTable, kChannelsTable, kMessagesTable, InsertSchema(1),
       InsertChannel(1, 1),
       InsertMessage(
           static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1)});
  SqliteBagReader overflow_reader;
  ASSERT_NO_THROW(overflow_reader.Open(overflow));
  EXPECT_THROW(
      overflow_reader.ReadMessages({}, [](BagMessage&&) { return true; }),
      ibmw::common::util::IbmwException);
}

TEST(StorageContentRoundTripTest, McapPreservesContentAndSequenceBoundary) {
  TemporaryDirectory temporary("ibmw-mcap-roundtrip");
  ExpectContentRoundTrip<McapStorage, McapBagReader>(
      temporary.Path() / "bag", "mcap");
}

TEST(StorageContentRoundTripTest, SqlitePreservesContentAndSequenceBoundary) {
  TemporaryDirectory temporary("ibmw-sqlite-roundtrip");
  ExpectContentRoundTrip<SqliteStorage, SqliteBagReader>(
      temporary.Path() / "bag", "sqlite3");
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
