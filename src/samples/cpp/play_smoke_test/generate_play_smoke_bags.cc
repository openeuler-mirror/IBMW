// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "HeaderSchema.hpp"
#include "cpp/util/buffer.h"
#include "cpp/util/schema_support.h"
#include "cpp/util/type_support.h"
#include "dds_types/util/dds_type_support.h"
#include "engine/transport/serialized_message.h"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "record_playback_ext/storage/mcap_storage.h"
#include "record_playback_ext/storage/sqlite_storage.h"
#include "ros2_types/util/ros2_schema_support.h"
#include "ros2_types/util/ros2_type_support_factory.h"

using Header = ibmw::protocols::common::Header;
using RosTestMsg = example_ros2::msg::RosTestMsg;
using ibmw::extensions::record_playback_extension::McapStorage;
using ibmw::extensions::record_playback_extension::SqliteStorage;
using ibmw::extensions::record_playback_extension::Storage;
using ibmw::extensions::record_playback_extension::StorageChannelDescriptor;
using ibmw::extensions::record_playback_extension::StorageOptions;
using ibmw::runtime::core::transport::SerializedMessage;
using ibmw::runtime::core::transport::TopicDescriptor;

IBMW_DDS_TYPE_NAME(Header, "ibmw::protocols::common::Header")

namespace {

constexpr uint64_t kStartNs = 100000000000ull;
constexpr uint64_t kStepNs = 100000000ull;

std::shared_ptr<ibmw::util::BufferArrayView> Own(std::string value) {
  auto storage = std::make_shared<std::string>(std::move(value));
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(storage->data(), storage->size()),
      [storage](const ibmw::util::BufferArrayView* view) { delete view; });
}

std::string SerializeHeader(uint64_t sequence, std::string payload) {
  Header message;
  message.time_stamp(kStartNs + sequence * kStepNs);
  message.frame_id(std::move(payload));
  message.sequence_num(sequence);
  ibmw::util::TypeSupportRef support(
      ibmw::dds::GetDdsMessageTypeSupport<Header>(
          "ibmw::protocols::common::Header"));
  return support.SimpleSerialize("cdr", &message);
}

std::string SerializeRos2(uint64_t sequence) {
  RosTestMsg message;
  message.num = static_cast<int64_t>(sequence);
  message.data = {1, 2, 3, 4};
  message.test_msg2.num = static_cast<int32_t>(sequence);
  message.test_msg2.num2 = 2.0;
  message.test_msg2.data = 7;
  ibmw::util::TypeSupportRef support(
      ibmw::GetRos2MessageTypeSupport<RosTestMsg>());
  return support.SimpleSerialize("ros2", &message);
}

template <typename StorageType>
void WriteCdrBag(const std::filesystem::path& output, std::string storage_id) {
  std::filesystem::remove_all(output);
  StorageType storage;
  storage.Open(StorageOptions{
      .output_path = output, .storage_id = std::move(storage_id)});
  auto schema = ibmw::util::SchemaSupportRef(
      ibmw::dds::GetDdsMessageSchemaSupport<Header>(
          "ibmw::protocols::common::Header", ibmw::dds::schema::kHeaderIdl,
          "ibmw::protocols::common::Header"))
                    .ToDescriptor();
  const auto schema_id = storage.RegisterSchema(schema);
  const auto make_channel = [&](std::string topic) {
    TopicDescriptor descriptor{
        .topic = std::move(topic),
        .msg_type = schema.type_name,
        .serialization_type = schema.serialization_type,
        .schema = schema,
        .qos = {.policies = {{"history", "keep_last"},
                             {"depth", "20"},
                             {"reliability", "reliable"},
                             {"durability", "volatile"}}}};
    return std::pair{
        storage.RegisterChannel(StorageChannelDescriptor{descriptor}, schema_id),
        std::move(descriptor)};
  };
  auto [data_channel, data_topic] = make_channel("/ibmw_play_smoke/data");
  auto [secondary_channel, secondary_topic] =
      make_channel("/ibmw_play_smoke/secondary");

  for (uint64_t index = 0; index < 10; ++index) {
    const auto timestamp = kStartNs + index * kStepNs;
    storage.Write(
        data_channel,
        SerializedMessage{
            .topic = data_topic,
            .identity = {.source_id = "play-smoke-fixture",
                         .publisher_id = "data",
                         .sequence = index},
            .publish_timestamp = timestamp,
            .receive_timestamp = timestamp,
            .source_driver = "fixture",
            .payload = Own(SerializeHeader(index, "data-" + std::to_string(index)))});
    storage.Write(
        secondary_channel,
        SerializedMessage{
            .topic = secondary_topic,
            .identity = {.source_id = "play-smoke-fixture",
                         .publisher_id = "secondary",
                         .sequence = index},
            .publish_timestamp = timestamp + 1000000ull,
            .receive_timestamp = timestamp + 1000000ull,
            .source_driver = "fixture",
            .payload = Own(SerializeHeader(
                100 + index, "secondary-" + std::to_string(100 + index)))});
  }
  storage.SetMessageStatistics(0, 0);
  storage.SetCloseStatus(true, {});
  storage.Close();
}

void WriteRos2Bag(const std::filesystem::path& output) {
  std::filesystem::remove_all(output);
  McapStorage storage;
  storage.Open(StorageOptions{.output_path = output, .storage_id = "mcap"});
  auto schema = ibmw::util::SchemaSupportRef(
      ibmw::ros2::GetRos2MessageSchemaSupport<RosTestMsg>())
                    .ToDescriptor();
  TopicDescriptor descriptor{
      .topic = "/ibmw_play_smoke/ros2",
      .msg_type = schema.type_name,
      .serialization_type = schema.serialization_type,
      .schema = schema,
      .qos = {.policies = {{"history", "keep_last"},
                           {"depth", "20"},
                           {"reliability", "reliable"},
                           {"durability", "volatile"}}}};
  const auto channel = storage.RegisterChannel(
      StorageChannelDescriptor{descriptor}, storage.RegisterSchema(schema));
  for (uint64_t index = 0; index < 10; ++index) {
    const auto timestamp = kStartNs + index * kStepNs;
    storage.Write(
        channel,
        SerializedMessage{
            .topic = descriptor,
            .identity = {.source_id = "play-smoke-fixture",
                         .publisher_id = "ros2",
                         .sequence = index},
            .publish_timestamp = timestamp,
            .receive_timestamp = timestamp,
            .source_driver = "fixture",
            .payload = Own(SerializeRos2(index))});
  }
  storage.SetMessageStatistics(0, 0);
  storage.SetCloseStatus(true, {});
  storage.Close();
}

void WriteOutOfOrderCdrBag(const std::filesystem::path& output) {
  std::filesystem::remove_all(output);
  const auto writer_config = output.parent_path() / ".reader-fixture-writer.yaml";
  {
    std::ofstream config(writer_config, std::ios::trunc);
    config << "chunk_size: 1\nno_chunking: false\n";
  }
  McapStorage storage;
  storage.Open(StorageOptions{
      .output_path = output,
      .storage_id = "mcap",
      .storage_config_file = writer_config});
  auto schema = ibmw::util::SchemaSupportRef(
      ibmw::dds::GetDdsMessageSchemaSupport<Header>(
          "ibmw::protocols::common::Header", ibmw::dds::schema::kHeaderIdl,
          "ibmw::protocols::common::Header"))
                    .ToDescriptor();
  TopicDescriptor descriptor{
      .topic = "/ibmw_play_smoke/data",
      .msg_type = schema.type_name,
      .serialization_type = schema.serialization_type,
      .schema = schema};
  const auto channel = storage.RegisterChannel(
      StorageChannelDescriptor{descriptor}, storage.RegisterSchema(schema));
  for (const uint64_t sequence : {0ull, 2ull, 1ull}) {
    // Use the same timestamp so BagPlayer's timestamp ordering cannot hide
    // the file order selected by the MCAP reader configuration.
    const auto timestamp = kStartNs;
    storage.Write(
        channel,
        SerializedMessage{
            .topic = descriptor,
            .identity = {.source_id = "play-smoke-reader-fixture",
                         .publisher_id = "data",
                         .sequence = sequence},
            .publish_timestamp = timestamp,
            .receive_timestamp = timestamp,
            .source_driver = "fixture",
            .payload = Own(SerializeHeader(
                sequence, "data-" + std::to_string(sequence)))});
  }
  storage.SetMessageStatistics(0, 0);
  storage.SetCloseStatus(true, {});
  storage.Close();
  std::filesystem::remove(writer_config);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: generate_play_smoke_bags <output-directory>\n";
    return 2;
  }
  try {
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    WriteCdrBag<McapStorage>(output / "cdr_mcap", "mcap");
    WriteCdrBag<SqliteStorage>(output / "cdr_sqlite3", "sqlite3");
    WriteOutOfOrderCdrBag(output / "cdr_mcap_reader");
    WriteRos2Bag(output / "ros2_mcap");
  } catch (const std::exception& error) {
    std::cerr << "generate play smoke bags failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
