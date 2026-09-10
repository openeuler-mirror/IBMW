// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/storage_factory.h"

#include <algorithm>
#include <memory>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

class TestStorage final : public Storage {
 public:
  void Open(const StorageOptions& options) override {
    summary_.output_path = options.output_path;
    open_ = true;
  }

  StorageSchemaId RegisterSchema(
      const ibmw::util::SchemaDescriptor& schema) override {
    static_cast<void>(schema);
    return 1;
  }

  StorageChannelId RegisterChannel(
      const StorageChannelDescriptor& channel,
      StorageSchemaId schema_id) override {
    static_cast<void>(channel);
    static_cast<void>(schema_id);
    return 1;
  }

  void Write(
      StorageChannelId channel_id,
      const ibmw::transport::SerializedMessage& message)
      override {
    static_cast<void>(channel_id);
    static_cast<void>(message);
  }

  void Flush() override {}

  void SetMessageStatistics(
      uint64_t dropped_message_count,
      uint64_t duplicate_message_count) override {
    summary_.dropped_message_count = dropped_message_count;
    summary_.duplicate_message_count = duplicate_message_count;
  }

  void Close() override { open_ = false; }
  bool IsOpen() const noexcept override { return open_; }
  const StorageSummary& Summary() const noexcept override { return summary_; }

 private:
  bool open_ = false;
  StorageSummary summary_;
};

TEST(StorageFactoryTest, BuiltinsAndCustomPluginAreDiscoverable) {
  constexpr auto kPluginId = "record-playback-test-storage";
  RegisterStoragePlugin(
      kPluginId, [] { return std::make_unique<TestStorage>(); });

  const auto plugins = AvailableStoragePlugins();
  EXPECT_TRUE(std::is_sorted(plugins.begin(), plugins.end()));
  EXPECT_NE(std::find(plugins.begin(), plugins.end(), "mcap"), plugins.end());
  EXPECT_NE(
      std::find(plugins.begin(), plugins.end(), "sqlite3"), plugins.end());
  EXPECT_NE(
      std::find(plugins.begin(), plugins.end(), kPluginId), plugins.end());

  auto storage = CreateStorage(kPluginId);
  ASSERT_NE(dynamic_cast<TestStorage*>(storage.get()), nullptr);
  storage->Open({.output_path = "/tmp/record-playback-test-storage"});
  EXPECT_TRUE(storage->IsOpen());
  EXPECT_EQ(
      storage->Summary().output_path,
      "/tmp/record-playback-test-storage");
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
