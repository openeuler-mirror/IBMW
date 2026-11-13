// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <utility>

#include "gtest/gtest.h"
#include "mcap/writer.hpp"
#include "cpp/util/type_support.h"
#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "record_playback_ext/playback_action.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

const ibmw_string_view_t kCdrSerialization[] = {{"cdr", 3}};

const ibmw_type_support_base_t* DummyTypeSupport() {
  static const std::string kTypeName = "dummy:PlaybackMessage";
  static const ibmw_type_support_base_t support{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView(kTypeName); },
      .create = [](void*) -> void* { return new uint8_t(0); },
      .destroy = [](void*, void* message) { delete static_cast<uint8_t*>(message); },
      .copy = [](void*, const void* from, void* to) {
        *static_cast<uint8_t*>(to) = *static_cast<const uint8_t*>(from);
      },
      .move = [](void*, void* from, void* to) {
        *static_cast<uint8_t*>(to) = *static_cast<uint8_t*>(from);
      },
      .serialize = nullptr,
      .deserialize = nullptr,
      .serialization_types_supported_num = [](void*) { return size_t{1}; },
      .serialization_types_supported_list =
          [](void*) { return kCdrSerialization; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &support;
}

std::filesystem::path WriteTemporaryBag() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("ibmw-playback-lock-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto mcap_path = root / "data_0.mcap";
  mcap::McapWriter writer;
  mcap::McapWriterOptions options("ibmw");
  if (!writer.open(mcap_path.string(), options).ok()) return {};
  mcap::Schema schema("PlaybackMessage", "omgidl",
                      "struct PlaybackMessage { octet data; }");
  writer.addSchema(schema);
  mcap::Channel channel("/playback_lock", "cdr", schema.id);
  writer.addChannel(channel);
  const uint64_t start = ibmw::common::util::GetCurTimestampNs() + 1000000;
  for (uint32_t sequence = 0; sequence < 128; ++sequence) {
    const std::byte payload[] = {std::byte{static_cast<unsigned char>(sequence)}};
    const auto status = writer.write(mcap::Message{
        .channelId = channel.id,
        .sequence = sequence,
        .logTime = start + sequence * 1000000,
        .publishTime = start + sequence * 1000000,
        .dataSize = sizeof(payload),
        .data = payload});
    if (!status.ok()) return {};
  }
  writer.close();
  std::ofstream metadata(root / "metadata.yaml");
  metadata << "ibmw_bagfile_information:\n"
           << "  version: 1\n"
           << "  topics:\n"
           << "    - id: 1\n      topic: /playback_lock\n"
           << "      msg_type: dummy:PlaybackMessage\n"
           << "      serialization_type: cdr\n"
           << "  files:\n    - path: data_0.mcap\n"
           << "      start_timestamp: " << start << "\n";
  return root;
}

// Even payloads on /playback_known, odd ones on a channel the summary lists
// under another id, so the read finds a channel the map does not hold.
std::filesystem::path WriteBagWithChannelMissingFromSummary() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("ibmw-playback-channel-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto mcap_path = root / "data_0.mcap";
  mcap::McapWriter writer;
  mcap::McapWriterOptions options("ibmw");
  options.compression = mcap::Compression::None;  // the patch below finds the record
  if (!writer.open(mcap_path.string(), options).ok()) return {};
  mcap::Schema schema("PlaybackMessage", "omgidl",
                      "struct PlaybackMessage { octet data; }");
  writer.addSchema(schema);
  mcap::Channel known("/playback_known", "cdr", schema.id);
  mcap::Channel hidden("/playback_hidden", "cdr", schema.id);
  writer.addChannel(known);
  writer.addChannel(hidden);
  const uint64_t start = ibmw::common::util::GetCurTimestampNs() + 1000000;
  for (uint32_t sequence = 0; sequence < 8; ++sequence) {
    const std::byte payload[] = {std::byte{static_cast<unsigned char>(sequence)}};
    const auto status = writer.write(mcap::Message{
        .channelId = sequence % 2 == 0 ? known.id : hidden.id,
        .sequence = sequence,
        .logTime = start + sequence * 1000000,
        .publishTime = start + sequence * 1000000,
        .dataSize = sizeof(payload),
        .data = payload});
    if (!status.ok()) return {};
  }
  writer.close();

  // A channel record starts with its u16 id, the u16 schema id and the
  // u32-prefixed topic; the last one is in the summary.
  std::string record{static_cast<char>(hidden.id), '\0', static_cast<char>(schema.id), '\0'};
  const uint32_t topic_size = hidden.topic.size();
  record.append(reinterpret_cast<const char*>(&topic_size), sizeof(topic_size));
  record += hidden.topic;
  std::fstream file(mcap_path, std::ios::in | std::ios::out | std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto position = bytes.rfind(record);
  if (position == std::string::npos || position == bytes.find(record)) return {};
  file.clear();
  file.seekp(static_cast<std::streamoff>(position));
  file.put(99);
  file.close();

  std::ofstream metadata(root / "metadata.yaml");
  metadata << "ibmw_bagfile_information:\n"
           << "  version: 1\n"
           << "  topics:\n"
           << "    - id: 1\n      topic: /playback_known\n"
           << "      msg_type: dummy:PlaybackMessage\n"
           << "      serialization_type: cdr\n"
           << "    - id: 2\n      topic: /playback_hidden\n"
           << "      msg_type: dummy:PlaybackMessage\n"
           << "      serialization_type: cdr\n"
           << "  files:\n    - path: data_0.mcap\n"
           << "      start_timestamp: " << start << "\n";
  return root;
}

TEST(PlaybackActionLockRegression, ConcurrentStopReadCloseCompletes) {
  const auto bag = WriteTemporaryBag();
  ASSERT_FALSE(bag.empty());
  runtime::core::executor::ThreadPoolExecutor executor;
  executor.Initialize("playback-lock-test", YAML::Load("{thread_num: 2}"));
  executor.Start();
  runtime::core::executor::ExecutorAccess access(&executor);
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage"
               ? ibmw::util::TypeSupportRef(DummyTypeSupport())
               : ibmw::util::TypeSupportRef();
  });
  action.RegisterGetExecutorFunc([&](std::string_view name) {
    return name == "playback-lock-test"
               ? ibmw::executor::ExecutorHandle(access.NativeHandle())
               : ibmw::executor::ExecutorHandle();
  });
  action.Initialize(YAML::Load(
      "{bag_path: '" + bag.string() +
      "', mode: signal, executor: playback-lock-test, topic_meta_list: "
      "[{topic: /playback_lock, msg_type: dummy:PlaybackMessage}]}"));
  action.InitExecutor();
  std::atomic<uint64_t> published{0};
  std::atomic<bool> first_publish{true};
  std::promise<void> publish_entered;
  auto publish_entered_future = publish_entered.get_future();
  std::promise<void> release_publish;
  auto release_publish_future = release_publish.get_future().share();
  action.RegisterPubRecordFunc([&](const PlaybackAction::OneRecord&) {
    published.fetch_add(1, std::memory_order_relaxed);
    if (first_publish.exchange(false, std::memory_order_relaxed)) {
      publish_entered.set_value();
      release_publish_future.wait();
    }
  });
  action.Start();
  ASSERT_TRUE(action.StartSignalPlayback(0, 0));
  ASSERT_EQ(publish_entered_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

  std::promise<void> stop_started;
  auto stop_started_future = stop_started.get_future();
  std::promise<void> close_started;
  auto close_started_future = close_started.get_future();
  auto stop = std::async(std::launch::async, [&] {
    stop_started.set_value();
    try {
      action.StopSignalPlayback();
    } catch (...) {
    }
  });
  auto close = std::async(std::launch::async, [&] {
    close_started.set_value();
    action.Shutdown();
  });
  const bool stop_was_started =
      stop_started_future.wait_for(std::chrono::seconds(1)) ==
      std::future_status::ready;
  const bool close_was_started =
      close_started_future.wait_for(std::chrono::seconds(1)) ==
      std::future_status::ready;
  release_publish.set_value();

  EXPECT_TRUE(stop_was_started);
  EXPECT_TRUE(close_was_started);
  const auto stop_status = stop.wait_for(std::chrono::seconds(5));
  const auto close_status = close.wait_for(std::chrono::seconds(5));
  EXPECT_EQ(stop_status, std::future_status::ready);
  EXPECT_EQ(close_status, std::future_status::ready);
  if (stop_status == std::future_status::ready) stop.get();
  if (close_status == std::future_status::ready) EXPECT_NO_THROW(close.get());
  executor.Shutdown();
  EXPECT_GT(published.load(std::memory_order_relaxed), 0u);
  std::filesystem::remove_all(bag);
}

// EXT-034: play_duration_s * 1e9 wraps to 5.12 ms for this value, which used
// to stop playback after a few of the 128 messages.
TEST(PlaybackActionLockRegression, HugePlayDurationDoesNotWrap) {
  const auto bag = WriteTemporaryBag();
  ASSERT_FALSE(bag.empty());
  runtime::core::executor::ThreadPoolExecutor executor;
  executor.Initialize("playback-duration-test", YAML::Load("{thread_num: 2}"));
  executor.Start();
  runtime::core::executor::ExecutorAccess access(&executor);
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage"
               ? ibmw::util::TypeSupportRef(DummyTypeSupport())
               : ibmw::util::TypeSupportRef();
  });
  action.RegisterGetExecutorFunc([&](std::string_view name) {
    return name == "playback-duration-test"
               ? ibmw::executor::ExecutorHandle(access.NativeHandle())
               : ibmw::executor::ExecutorHandle();
  });
  action.Initialize(YAML::Load(
      "{bag_path: '" + bag.string() +
      "', mode: signal, executor: playback-duration-test, topic_meta_list: "
      "[{topic: /playback_lock, msg_type: dummy:PlaybackMessage}]}"));
  action.InitExecutor();
  std::atomic<uint64_t> published{0};
  action.RegisterPubRecordFunc([&](const PlaybackAction::OneRecord&) {
    published.fetch_add(1, std::memory_order_relaxed);
  });
  action.Start();
  ASSERT_TRUE(action.StartSignalPlayback(0, 29549378163073488u));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (published.load() < 128 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(published.load(), 128u);
  action.Shutdown();
  executor.Shutdown();
  std::filesystem::remove_all(bag);
}

// EXT-033: a signal playback can start again after a stop or after its end;
// a stop drops the publishes already queued, and they never mix into the
// next playback.
TEST(PlaybackActionLockRegression, SignalPlaybackRestarts) {
  const auto bag = WriteTemporaryBag();
  ASSERT_FALSE(bag.empty());
  runtime::core::executor::ThreadPoolExecutor executor;
  executor.Initialize("playback-restart-test", YAML::Load("{thread_num: 2}"));
  executor.Start();
  runtime::core::executor::ExecutorAccess access(&executor);
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage"
               ? ibmw::util::TypeSupportRef(DummyTypeSupport())
               : ibmw::util::TypeSupportRef();
  });
  action.RegisterGetExecutorFunc([&](std::string_view name) {
    return name == "playback-restart-test"
               ? ibmw::executor::ExecutorHandle(access.NativeHandle())
               : ibmw::executor::ExecutorHandle();
  });
  action.Initialize(YAML::Load(
      "{bag_path: '" + bag.string() +
      "', mode: signal, executor: playback-restart-test, topic_meta_list: "
      "[{topic: /playback_lock, msg_type: dummy:PlaybackMessage}]}"));
  action.InitExecutor();
  std::mutex mutex;
  std::vector<int> payloads;
  action.RegisterPubRecordFunc([&](const PlaybackAction::OneRecord& record) {
    const auto data = record.buffer_view_ptr->JoinToString();
    std::lock_guard<std::mutex> lock(mutex);
    payloads.push_back(static_cast<unsigned char>(data.at(0)));
  });
  // Records are dispatched one task each; with two executor threads records
  // due at the same time may publish in either order, so compare sorted.
  const auto take = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    auto result = std::exchange(payloads, {});
    std::sort(result.begin(), result.end());
    return result;
  };
  const auto wait_for = [&](size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (payloads.size() >= count) return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  std::vector<int> all(128);
  for (int i = 0; i < 128; ++i) all[i] = i;
  action.Start();

  // Stopped early: the 128 queued publishes (one batch) are dropped.
  ASSERT_TRUE(action.StartSignalPlayback(0, 0));
  wait_for(1);
  action.StopSignalPlayback();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_LT(take().size(), 64u);

  // Started again: the whole bag once, nothing from the first run.
  ASSERT_TRUE(action.StartSignalPlayback(0, 0));
  wait_for(128);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // and the end
  EXPECT_EQ(take(), all);

  // After the natural end it starts once more.
  ASSERT_TRUE(action.StartSignalPlayback(0, 0));
  wait_for(128);
  EXPECT_EQ(take(), all);
  action.Shutdown();
  executor.Shutdown();
  std::filesystem::remove_all(bag);
}

// EXT-032: a message of an unmapped channel is skipped, not published as a
// topic, and the playback goes on.
TEST(PlaybackActionLockRegression, SkipsMessagesOfAChannelMissingFromTheSummary) {
  const auto bag = WriteBagWithChannelMissingFromSummary();
  ASSERT_FALSE(bag.empty());
  runtime::core::executor::ThreadPoolExecutor executor;
  executor.Initialize("playback-channel-test", YAML::Load("{thread_num: 2}"));
  executor.Start();
  runtime::core::executor::ExecutorAccess access(&executor);
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage"
               ? ibmw::util::TypeSupportRef(DummyTypeSupport())
               : ibmw::util::TypeSupportRef();
  });
  action.RegisterGetExecutorFunc([&](std::string_view name) {
    return name == "playback-channel-test"
               ? ibmw::executor::ExecutorHandle(access.NativeHandle())
               : ibmw::executor::ExecutorHandle();
  });
  action.Initialize(YAML::Load(
      "{bag_path: '" + bag.string() +
      "', mode: signal, executor: playback-channel-test, topic_meta_list: "
      "[{topic: /playback_known, msg_type: dummy:PlaybackMessage}, "
      "{topic: /playback_hidden, msg_type: dummy:PlaybackMessage}]}"));
  action.InitExecutor();
  std::mutex mutex;
  std::vector<int> payloads;
  action.RegisterPubRecordFunc([&](const PlaybackAction::OneRecord& record) {
    const auto data = record.buffer_view_ptr->JoinToString();
    std::lock_guard<std::mutex> lock(mutex);
    payloads.push_back(static_cast<unsigned char>(data.at(0)));
  });
  action.Start();
  ASSERT_TRUE(action.StartSignalPlayback(0, 0));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (payloads.size() >= 4) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a wrong odd one would follow
  action.Shutdown();
  executor.Shutdown();
  std::sort(payloads.begin(), payloads.end());
  EXPECT_EQ(payloads, (std::vector<int>{0, 2, 4, 6}));
  std::filesystem::remove_all(bag);
}

TEST(PlaybackActionLockRegression, DoesNotManuallyUnlockGuardedMutexes) {
  const auto source_path =
      std::filesystem::path(__FILE__).parent_path() / "playback_action.cc";
  std::ifstream source(source_path);
  ASSERT_TRUE(source.is_open()) << source_path;
  const std::string contents{std::istreambuf_iterator<char>(source),
                             std::istreambuf_iterator<char>()};
  for (const std::string mutex_name : {"playback_state_mutex_", "db_mutex_"}) {
    EXPECT_EQ(contents.find(mutex_name + ".unlock()"), std::string::npos)
        << "A lock_guard-owned mutex must not be unlocked manually: " << mutex_name;
  }
}

TEST(PlaybackActionLockRegression, RejectsMetadataFilesOutsideBagRoot) {
  const auto bag = WriteTemporaryBag();
  ASSERT_FALSE(bag.empty());
  const auto outside = bag.parent_path() / "outside.mcap";
  std::ofstream(outside) << "outside";
  const auto metadata_path = bag / "metadata.yaml";
  std::ifstream input(metadata_path);
  std::string metadata((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  const auto marker = metadata.find("path: data_0.mcap");
  ASSERT_NE(marker, std::string::npos);
  metadata.replace(marker, std::string("path: data_0.mcap").size(),
                   "path: ../outside.mcap");
  std::ofstream(metadata_path) << metadata;
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage"
               ? ibmw::util::TypeSupportRef(DummyTypeSupport())
               : ibmw::util::TypeSupportRef();
  });
  EXPECT_THROW(
      action.Initialize(YAML::Load(
          "{bag_path: '" + bag.string() +
          "', mode: signal, executor: unused, topic_meta_list: []}")),
      ibmw::common::util::IbmwException);
  std::filesystem::remove_all(bag);
}

// EXT-044: files that metadata.yaml lists but that are missing or are not
// regular files are skipped; with none left, Initialize fails.
TEST(PlaybackActionLockRegression, SkipsMissingAndNonRegularFiles) {
  const auto bag = WriteTemporaryBag();
  ASSERT_FALSE(bag.empty());
  std::filesystem::create_directories(bag / "folder.mcap");
  const auto metadata_path = bag / "metadata.yaml";
  auto metadata = YAML::LoadFile(metadata_path.string());
  auto files = metadata["ibmw_bagfile_information"]["files"];
  files[0]["path"] = "missing.mcap";
  YAML::Node folder;
  folder["path"] = "folder.mcap";
  folder["start_timestamp"] = files[0]["start_timestamp"];
  files.push_back(folder);
  std::ofstream(metadata_path) << metadata;
  PlaybackAction action;
  action.RegisterGetTypeSupportFunc([](std::string_view type) {
    return type == "dummy:PlaybackMessage" ? ibmw::util::TypeSupportRef(DummyTypeSupport())
                                           : ibmw::util::TypeSupportRef();
  });
  try {
    action.Initialize(YAML::Load("{bag_path: '" + bag.string() +
                                 "', mode: signal, executor: unused, topic_meta_list: []}"));
    ADD_FAILURE() << "a bag without usable files was accepted";
  } catch (const ibmw::common::util::IbmwException& e) {
    EXPECT_NE(std::string(e.what()).find("Can not find useful bag file"), std::string::npos) << e.what();
  }
  std::filesystem::remove_all(bag);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
