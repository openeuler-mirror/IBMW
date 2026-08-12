// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h>

#include "cpp/util/schema_support.h"
#include "ibmw_msgs/msg/joint_command_array.hpp"
#include "record_playback_ext/converter/bag_converter.h"
#include "record_playback_ext/converter/rosbag2_adapter.h"
#include "type_support/ros2/ros2_schema_support.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic<uint64_t> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("ibmw-ros2-schema-equivalence-" +
             std::to_string(::getpid()) + "-" +
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

std::shared_ptr<ibmw::util::BufferArrayView> TestPayload() {
  auto bytes = std::make_shared<std::string>("serialized-payload");
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(bytes->data(), bytes->size()),
      [bytes](const ibmw::util::BufferArrayView* view) { delete view; });
}

class SingleMessageBagReader final : public BagReader {
 public:
  explicit SingleMessageBagReader(ibmw::util::SchemaDescriptor schema) {
    info_.bag_path = "in-memory-schema-source";
    info_.storage_id = "memory";
    info_.schemas.emplace_back(
        BagSchemaInfo{.id = 0, .descriptor = schema, .hash_valid = true});
    info_.channels.emplace_back(BagChannelInfo{
        .id = 0,
        .schema_id = 0,
        .topic = {.topic = "/schema_equivalence",
                  .msg_type = schema.type_name,
                  .serialization_type = schema.serialization_type,
                  .schema = std::move(schema)},
        .message_count = 1});
    info_.start_timestamp = 100;
    info_.end_timestamp = 100;
    info_.message_count = 1;
    info_.closed_cleanly = true;
  }

  void Open(const std::filesystem::path&, const ReaderOpenOptions&) override {}

  void ReadMessages(
      const BagReadOptions&,
      const MessageCallback& callback) const override {
    static_cast<void>(callback(BagMessage{
        .channel = info_.channels.front(),
        .log_time = 100,
        .publish_time = 100,
        .sequence = 0,
        .payload = TestPayload()}));
  }

  const BagInfo& Info() const noexcept override { return info_; }

 private:
  BagInfo info_;
};

void ExpectEquivalentSchema(
    const ibmw::util::SchemaDescriptor& expected,
    const ibmw::util::SchemaDescriptor& actual) {
  EXPECT_EQ(actual.type_name, expected.type_name);
  EXPECT_EQ(actual.serialization_type, expected.serialization_type);
  EXPECT_EQ(actual.schema_name, expected.schema_name);
  EXPECT_EQ(actual.schema_encoding, expected.schema_encoding);
  EXPECT_EQ(actual.schema_data, expected.schema_data);
  EXPECT_EQ(actual.schema_hash, expected.schema_hash);
}

TEST(Rosbag2SchemaEquivalence, TypeSupportExportAndImportMatchExactly) {
  const ibmw::util::SchemaSupportRef support(
      ibmw::ros2::GetRos2MessageSchemaSupport<
          ibmw_msgs::msg::JointCommandArray>());
  ASSERT_TRUE(support);
  const auto expected = support.ToDescriptor();
  ASSERT_FALSE(expected.schema_data.empty());
  ASSERT_EQ(
      expected.schema_hash,
      ibmw::util::ComputeSchemaHash(expected.schema_data));

  TemporaryDirectory temporary;
  const auto output_path = temporary.Path() / "rosbag2";
  const SingleMessageBagReader source(expected);
  const auto result = ConvertBagToRosbag2(
      source,
      StorageOptions{.output_path = output_path, .storage_id = "sqlite3"});
  ASSERT_EQ(result.output_message_count, 1u);
  ASSERT_EQ(result.schema_count, 1u);
  ASSERT_EQ(result.channel_count, 1u);

  Rosbag2BagReader imported;
  imported.Open(output_path);
  ASSERT_EQ(imported.Info().schemas.size(), 1u);
  ASSERT_EQ(imported.Info().channels.size(), 1u);
  ExpectEquivalentSchema(
      expected, imported.Info().schemas.front().descriptor);
  ExpectEquivalentSchema(
      expected, imported.Info().channels.front().topic.schema);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
