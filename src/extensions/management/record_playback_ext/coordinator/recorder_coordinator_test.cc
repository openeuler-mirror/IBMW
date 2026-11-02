// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/recorder_coordinator.h"

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

// Keeps what the coordinator hands to the storage.
class FakeStorage : public Storage {
 public:
  struct Record {
    std::map<std::string, uint64_t> topic_messages;
    uint64_t schemas = 0;
    uint64_t dropped = 0;
    // When valid, every write waits for it: a stuck disk.
    std::shared_future<void> write_gate;
  };

  explicit FakeStorage(Record* record) : record_(record) {}

  void Open(const StorageOptions&) override { open_ = true; }
  StorageSchemaId RegisterSchema(const ibmw::util::SchemaDescriptor&) override {
    return static_cast<StorageSchemaId>(++record_->schemas);
  }
  StorageChannelId RegisterChannel(const StorageChannelDescriptor& channel, StorageSchemaId) override {
    channels_[++next_channel_] = channel.topic.topic;
    return next_channel_;
  }
  void Write(StorageChannelId channel_id, const ibmw::transport::SerializedMessage&) override {
    if (record_->write_gate.valid()) record_->write_gate.wait();
    ++record_->topic_messages[channels_.at(channel_id)];
  }
  void Flush() override {}
  void SetMessageStatistics(uint64_t dropped, uint64_t) override { record_->dropped = dropped; }
  void Close() override { open_ = false; }
  bool IsOpen() const noexcept override { return open_; }
  const StorageSummary& Summary() const noexcept override { return summary_; }

 private:
  Record* record_;
  bool open_ = false;
  StorageChannelId next_channel_ = 0;
  std::map<StorageChannelId, std::string> channels_;
  StorageSummary summary_;
};

ibmw::transport::TopicDescriptor MakeTopic(std::string topic, std::string msg_type, std::string schema_data) {
  ibmw::transport::TopicDescriptor descriptor;
  descriptor.topic = std::move(topic);
  descriptor.msg_type = msg_type;
  descriptor.serialization_type = "cdr";
  descriptor.schema.type_name = std::move(msg_type);
  descriptor.schema.serialization_type = "cdr";
  descriptor.schema.schema_name = descriptor.schema.type_name;
  descriptor.schema.schema_encoding = "ros2msg";
  descriptor.schema.schema_hash = ibmw::util::ComputeSchemaHash(schema_data);
  descriptor.schema.schema_data = std::move(schema_data);
  return descriptor;
}

ibmw::transport::SerializedMessage MakeMessage(const ibmw::transport::TopicDescriptor& topic) {
  ibmw::transport::SerializedMessage message;
  static char payload[] = "payload";
  message.topic = topic;
  message.payload = std::make_shared<ibmw::util::BufferArrayView>(payload, sizeof(payload));
  return message;
}

// EXT-040: two publishers of one type with different schema versions (a
// rolling upgrade) only lose the messages of the conflicting topic; the
// writer keeps recording every other topic.
TEST(RecorderCoordinatorTest, ConflictingSchemaOnlyDropsThatTopic) {
  FakeStorage::Record record;
  RecorderCoordinator::Options options;
  options.queue.full_policy = RecordQueue::FullPolicy::kBlock;
  RecorderCoordinator coordinator(options, std::make_unique<FakeStorage>(&record));
  coordinator.Initialize();
  coordinator.Start();

  const auto v1 = MakeTopic("/old", "pkg/msg/Pose", "float64 x");
  const auto v2 = MakeTopic("/new", "pkg/msg/Pose", "float64 x\nfloat64 y");
  const auto other = MakeTopic("/other", "pkg/msg/Other", "int32 a");
  // A conflicting channel announced without a publisher is skipped as well.
  EXPECT_TRUE(coordinator.RegisterChannel(StorageChannelDescriptor{.topic = v1}));
  EXPECT_FALSE(coordinator.RegisterChannel(StorageChannelDescriptor{.topic = v2}));
  for (int i = 0; i < 3; ++i) {
    for (const auto* topic : {&v1, &v2, &other}) {
      EXPECT_EQ(coordinator.Accept(MakeMessage(*topic)), RecordQueue::PushResult::kPushed);
    }
  }
  EXPECT_NO_THROW(coordinator.Close());

  EXPECT_EQ(record.topic_messages["/old"], 3u);
  EXPECT_EQ(record.topic_messages["/other"], 3u);
  EXPECT_EQ(record.topic_messages.count("/new"), 0u);
  EXPECT_EQ(record.schemas, 2u);
  EXPECT_EQ(record.dropped, 3u);
  EXPECT_EQ(coordinator.SchemaConflictMessages(), 3u);
}

// EXT-041: a snapshot behind a stuck storage write gives up waiting after the
// timeout, so the caller (the bag command loop) still handles stop requests.
TEST(RecorderCoordinatorTest, SnapshotWaitIsBounded) {
  FakeStorage::Record record;
  std::promise<void> disk;
  record.write_gate = disk.get_future().share();
  RecorderCoordinator::Options options;
  options.snapshot_mode = true;
  RecorderCoordinator coordinator(options, std::make_unique<FakeStorage>(&record));
  coordinator.Initialize();
  coordinator.Start();

  const auto topic = MakeTopic("/snap", "pkg/msg/Pose", "float64 x");
  ASSERT_EQ(coordinator.Accept(MakeMessage(topic)), RecordQueue::PushResult::kPushed);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(coordinator.TriggerSnapshot(std::chrono::milliseconds(100)));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));

  disk.set_value();
  EXPECT_TRUE(coordinator.TriggerSnapshot(std::chrono::seconds(5)));
  EXPECT_NO_THROW(coordinator.Close());
  EXPECT_EQ(record.topic_messages["/snap"], 1u);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
