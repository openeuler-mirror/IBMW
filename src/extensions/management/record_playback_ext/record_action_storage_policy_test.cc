// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for EXT-021: optional disk usage limits of the recorder
// (min_free_space_m, max_folder_num, max_total_size_m), all off by default.

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <future>
#include <iterator>
#include <memory>
#include <thread>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time_util.h"
#include "engine/executor/executor_access.h"
#include "engine/executor/thread_pool_executor.h"
#include "mcap/reader.hpp"
#include "record_playback_ext/record_action.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

namespace fs = std::filesystem;

class RecordActionStoragePolicyTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("ibmw_storage_policy_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }

  void TearDown() override { fs::remove_all(root_); }

  static void WriteFile(const fs::path& path, size_t size) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << std::string(size, 'x');
  }

  // Lists the entries directly under `dir`, sorted.
  static std::vector<std::string> List(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
  }

  // Initializes an imd recorder on `bag_path` (it opens a bag folder and
  // file at once), shuts it down and returns the parsed storage policy.
  static RecordAction::Options::StoragePolicy Record(const fs::path& bag_path, const std::string& storage_policy) {
    YAML::Node options = YAML::Load("{mode: imd, executor: none, topic_meta_list: []}");
    options["bag_path"] = bag_path.string();
    options["storage_policy"] = YAML::Load(storage_policy);

    RecordAction action;
    action.RegisterGetExecutorFunc([](std::string_view) { return ibmw::executor::ExecutorHandle(); });
    action.RegisterGetTypeSupportFunc([](std::string_view) { return ibmw::util::TypeSupportRef(); });
    action.RegisterGetSchemaSupportFunc(
        [](std::string_view, std::string_view) -> std::optional<ibmw::util::SchemaDescriptor> {
          return std::nullopt;
        });
    action.Initialize(options);
    action.Shutdown();
    return action.GetOptions().storage_policy;
  }

  fs::path root_;
};

TEST_F(RecordActionStoragePolicyTest, LimitsAreOffByDefaultAndParsed) {
  RecordAction::Options::StoragePolicy defaults;
  EXPECT_EQ(defaults.min_free_space_m, 0u);
  EXPECT_EQ(defaults.max_folder_num, 0u);
  EXPECT_EQ(defaults.max_total_size_m, 0u);

  const auto policy = Record(root_ / "bags", "{min_free_space_m: 1, max_folder_num: 2, max_total_size_m: 3}");
  EXPECT_EQ(policy.min_free_space_m, 1u);
  EXPECT_EQ(policy.max_folder_num, 2u);
  EXPECT_EQ(policy.max_total_size_m, 3u);
}

TEST_F(RecordActionStoragePolicyTest, OldestBagFoldersAreRemoved) {
  const auto bags = root_ / "bags";
  for (const char* name : {"ibmwbag_20200101_000000", "ibmwbag_20200102_000000", "ibmwbag_20200103_000000"})
    WriteFile(bags / name / "a.mcap", 1);
  // Not created by the recorder: never touched.
  WriteFile(bags / "ibmwbag_2020010_0000000" / "a.mcap", 1);
  WriteFile(bags / "my_ibmwbag_20200101_000000" / "a.mcap", 1);
  WriteFile(root_ / "outside" / "a.mcap", 1);
  fs::create_directory_symlink(root_ / "outside", bags / "ibmwbag_19990101_000000");
  // A dangling link must not stop the pruning either.
  fs::create_directory_symlink(root_ / "missing", bags / "ibmwbag_19980101_000000");

  Record(bags, "{max_folder_num: 2}");

  const auto names = List(bags);
  ASSERT_EQ(names.size(), 6u) << ::testing::PrintToString(names);
  EXPECT_EQ(names[0], "ibmwbag_19980101_000000");  // the dangling link
  EXPECT_EQ(names[1], "ibmwbag_19990101_000000");  // the link
  EXPECT_EQ(names[2], "ibmwbag_20200103_000000");  // newest old folder
  EXPECT_EQ(names[3], "ibmwbag_2020010_0000000");
  EXPECT_NE(names[4], "ibmwbag_20200101_000000");  // the new folder
  EXPECT_EQ(names[5], "my_ibmwbag_20200101_000000");
  EXPECT_TRUE(fs::exists(root_ / "outside" / "a.mcap"));
}

TEST_F(RecordActionStoragePolicyTest, FoldersAreKeptWithoutLimit) {
  const auto bags = root_ / "bags";
  for (const char* name : {"ibmwbag_20200101_000000", "ibmwbag_20200102_000000"})
    WriteFile(bags / name / "a.mcap", 1);
  Record(bags, "{}");
  EXPECT_EQ(List(bags).size(), 3u);
}

// EXT-023: bag folder names have second precision; a folder started in a
// second whose name is taken gets a numbered suffix instead of failing.
TEST_F(RecordActionStoragePolicyTest, FolderOfTheSameSecondGetsASuffix) {
  const auto bags = root_ / "bags";
  std::vector<std::string> taken;
  const time_t now = ::time(nullptr);
  for (time_t t = now; t < now + 3; ++t) {  // the recorder starts within these seconds
    struct tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "ibmwbag_%Y%m%d_%H%M%S", &tm);
    taken.emplace_back(buf);
    fs::create_directories(bags / buf);
  }

  ASSERT_NO_THROW(Record(bags, "{}"));

  const auto names = List(bags);
  ASSERT_EQ(names.size(), taken.size() + 1) << ::testing::PrintToString(names);
  std::vector<std::string> created;
  std::set_difference(names.begin(), names.end(), taken.begin(), taken.end(), std::back_inserter(created));
  ASSERT_EQ(created.size(), 1u);
  EXPECT_EQ(created[0].size(), taken[0].size() + 3) << created[0];
  EXPECT_EQ(created[0].substr(taken[0].size()), "_01") << created[0];
  for (const auto& name : taken) EXPECT_TRUE(fs::is_empty(bags / name)) << name;  // not reused
}

TEST_F(RecordActionStoragePolicyTest, NumberedBagFoldersArePruned) {
  const auto bags = root_ / "bags";
  for (const char* name : {"ibmwbag_20200101_000000", "ibmwbag_20200101_000000_01", "ibmwbag_20200101_000000_02"})
    WriteFile(bags / name / "a.mcap", 1);
  WriteFile(bags / "ibmwbag_20200101_000000_1" / "a.mcap", 1);  // not a recorder name

  Record(bags, "{max_folder_num: 2}");

  const auto names = List(bags);
  ASSERT_EQ(names.size(), 3u) << ::testing::PrintToString(names);
  EXPECT_EQ(names[0], "ibmwbag_20200101_000000_02");  // newest old folder
  EXPECT_EQ(names[1], "ibmwbag_20200101_000000_1");
}

TEST_F(RecordActionStoragePolicyTest, OldestBagFilesAreRemovedAboveTotalSize) {
  const auto dir = root_ / "bag";
  std::deque<MetaData::FileMeta> files;
  for (const char* name : {"1.mcap", "2.mcap", "3.mcap", "4.mcap"}) {
    WriteFile(dir / name, 400);
    files.push_back({.path = name, .start_timestamp = 0});
  }
  bag_retention::PruneBagFiles(dir, files, 1000);
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(files[0].path, "3.mcap");
  EXPECT_EQ(List(dir), (std::vector<std::string>{"3.mcap", "4.mcap"}));

  // The current (last) file is kept even if it alone is above the limit.
  WriteFile(dir / "4.mcap", 5000);
  bag_retention::PruneBagFiles(dir, files, 1000);
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files[0].path, "4.mcap");
  EXPECT_EQ(List(dir), (std::vector<std::string>{"4.mcap"}));
}

// No disk has 4 PB free: writing pauses and no bag file is created.
TEST_F(RecordActionStoragePolicyTest, LowFreeSpacePausesWriting) {
  const auto bags = root_ / "bags";
  Record(bags, "{min_free_space_m: 4294967295}");
  const auto folders = List(bags);
  ASSERT_EQ(folders.size(), 1u);
  for (const auto& entry : fs::recursive_directory_iterator(bags))
    EXPECT_NE(entry.path().extension(), ".mcap") << entry.path();

  Record(root_ / "bags2", "{min_free_space_m: 1}");
  bool has_mcap = false;
  for (const auto& entry : fs::recursive_directory_iterator(root_ / "bags2"))
    has_mcap = has_mcap || entry.path().extension() == ".mcap";
  EXPECT_TRUE(has_mcap);
}

// A folder newer than the one kept (another day started meanwhile) is
// never removed, even if the limit is exceeded.
TEST_F(RecordActionStoragePolicyTest, NewerFoldersAreNeverRemoved) {
  const auto bags = root_ / "bags";
  for (const char* name : {"ibmwbag_20200101_000000", "ibmwbag_20200102_000000", "ibmwbag_20200103_000000"})
    WriteFile(bags / name / "a.mcap", 1);
  bag_retention::PruneBagFolders(bags, bags / "ibmwbag_20200102_000000", 1);
  EXPECT_EQ(List(bags), (std::vector<std::string>{"ibmwbag_20200102_000000", "ibmwbag_20200103_000000"}));
}

// Recording through the record executor, with a controllable free space.
class RecordActionFreeSpaceTest : public RecordActionStoragePolicyTest {
 protected:
  void SetUp() override {
    RecordActionStoragePolicyTest::SetUp();
    executor_.Initialize("storage-policy-test", YAML::Load("{thread_num: 1}"));
    executor_.Start();
    access_ = std::make_unique<runtime::core::executor::ExecutorAccess>(&executor_);
  }

  void TearDown() override {
    executor_.Shutdown();
    RecordActionStoragePolicyTest::TearDown();
  }

  ibmw::executor::ExecutorHandle Handle() const { return ibmw::executor::ExecutorHandle(access_->NativeHandle()); }

  void Initialize(RecordAction& action, const std::string& mode, const std::string& storage_policy,
                  const std::string& extra_options = "{}") {
    action.RegisterGetExecutorFunc([this](std::string_view) { return Handle(); });
    // A type with no serialization of its own: topics are recorded through the schema below.
    static const ibmw_type_support_base_t kType{
        .serialization_types_supported_num = [](void*) -> size_t { return 0; },
        .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return nullptr; }};
    action.RegisterGetTypeSupportFunc([](std::string_view) { return ibmw::util::TypeSupportRef(&kType); });
    action.RegisterGetSchemaSupportFunc(
        [](std::string_view type, std::string_view serialization) -> std::optional<ibmw::util::SchemaDescriptor> {
          return ibmw::util::SchemaDescriptor{.type_name = std::string(type),
                                              .serialization_type = std::string(serialization),
                                              .schema_name = std::string(type),
                                              .schema_encoding = "jsonschema",
                                              .schema_data = "{}",
                                              .schema_hash = ibmw::util::ComputeSchemaHash("{}")};
        });
    action.SetFreeSpaceQuery([this](const fs::path&) { return free_space_.load(); });
    YAML::Node options = YAML::Load("{executor: storage-policy-test, topic_meta_list: []}");
    options["mode"] = mode;
    options["bag_path"] = (root_ / "bags").string();
    options["storage_policy"] = YAML::Load(storage_policy);
    for (const auto& kv : YAML::Load(extra_options)) options[kv.first.as<std::string>()] = kv.second;
    action.Initialize(options);
    action.InitExecutor(Handle());
    action.Start();
  }

  // Waits until the record executor ran everything queued so far.
  void Drain() {
    std::promise<void> done;
    auto future = done.get_future();
    Handle().Dispatch([&done] { done.set_value(); });
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  }

  void Add(RecordAction& action, uint64_t log_timestamp, uint64_t topic_index = 1, uint32_t sequence = 0) {
    action.AddRecord({.topic_index = topic_index,
                      .log_timestamp = log_timestamp,
                      .pub_timestamp = log_timestamp,
                      .pub_sequence = sequence,
                      .buffer_view_ptr = std::make_shared<ibmw::util::BufferArrayView>(payload_.data(), payload_.size())});
  }

  std::vector<fs::path> McapFiles() const {
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root_ / "bags"))
      if (entry.path().extension() == ".mcap") files.push_back(entry.path());
    return files;
  }

  static constexpr uintmax_t kPlenty = uintmax_t{1} << 40;
  std::atomic<uintmax_t> free_space_{kPlenty};
  const std::string payload_ = std::string(600 * 1024, 'p');
  runtime::core::executor::ThreadPoolExecutor executor_;
  std::unique_ptr<runtime::core::executor::ExecutorAccess> access_;
};

// The disk fills up at a bag split: the split must pause writing (not
// crash), and writing resumes with a new file once there is space again.
TEST_F(RecordActionFreeSpaceTest, SplitWhileShortOfSpacePausesAndResumes) {
  RecordAction action;
  Initialize(action, "imd", "{max_bag_size_m: 1, min_free_space_m: 100}");
  const uint64_t now = ibmw::common::util::GetCurTimestampNs();
  Add(action, now);
  Add(action, now);
  Drain();
  ASSERT_EQ(McapFiles().size(), 1u);

  free_space_ = 0;
  Add(action, now + 1'000'000'000);  // reaches max_bag_size: split
  Add(action, now + 1'000'000'000);
  Drain();
  EXPECT_EQ(McapFiles().size(), 1u);  // paused: no new file

  free_space_ = kPlenty;
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // next free space check
  Add(action, now + 5'000'000'000);
  Drain();
  EXPECT_EQ(McapFiles().size(), 2u);  // resumed with a new file
  action.Shutdown();
}

// While short of space a signal record fails to start (no empty folder is
// created), and a failed start does not block the next one.
TEST_F(RecordActionFreeSpaceTest, SignalRecordDoesNotStartWhileShortOfSpace) {
  RecordAction action;
  Initialize(action, "signal", "{min_free_space_m: 100}");
  std::string folder;
  ASSERT_TRUE(action.StartSignalRecord(0, 60, folder));
  ASSERT_TRUE(action.StopSignalRecord());
  EXPECT_EQ(List(root_ / "bags").size(), 1u);

  free_space_ = 0;
  EXPECT_ANY_THROW(action.StartSignalRecord(0, 60, folder));
  EXPECT_EQ(List(root_ / "bags").size(), 1u);

  free_space_ = kPlenty;
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // folder names have seconds
  ASSERT_TRUE(action.StartSignalRecord(0, 60, folder));
  EXPECT_EQ(List(root_ / "bags").size(), 2u);
  action.Shutdown();
}

// EXT-029: max_preparation_cache_m bounds the signal pre-record cache by
// size; the oldest records go first. 0 keeps only the time window.
TEST_F(RecordActionFreeSpaceTest, PreparationCacheIsBoundedBySize) {
  for (const uint32_t limit_m : {0u, 1u}) {
    RecordAction action;
    Initialize(action, "signal", "{}",
               "{max_preparation_duration_s: 100, max_preparation_cache_m: " + std::to_string(limit_m) +
                   ", topic_meta_list: [{topic: /t, msg_type: t, serialization_type: json}]}");
    const uint64_t now = ibmw::common::util::GetCurTimestampNs();
    for (uint32_t i = 0; i < 5; ++i) Add(action, now - (5 - i) * 1'000'000'000ull, 0, i);  // 600 KiB each
    Drain();
    std::string folder;
    ASSERT_TRUE(action.StartSignalRecord(100, 60, folder));
    ASSERT_TRUE(action.StopSignalRecord());
    action.Shutdown();

    std::vector<uint32_t> sequences;
    for (const auto& file : McapFiles()) {
      mcap::McapReader reader;
      ASSERT_TRUE(reader.open(file.string()).ok());
      for (const auto& view : reader.readMessages()) sequences.push_back(view.message.sequence);
      reader.close();
    }
    if (limit_m == 0) {
      EXPECT_EQ(sequences, (std::vector<uint32_t>{0, 1, 2, 3, 4}));
    } else {
      EXPECT_EQ(sequences, (std::vector<uint32_t>{4}));
    }
    fs::remove_all(root_ / "bags");
  }
}

// EXT-026: a file that can not be opened pauses writing like a full disk: the
// next attempt comes after a second, not after another bag worth of records.
TEST_F(RecordActionFreeSpaceTest, FailedOpenPausesAndRetries) {
  RecordAction action;
  Initialize(action, "imd", "{max_bag_size_m: 1}");
  const uint64_t now = ibmw::common::util::GetCurTimestampNs();
  Add(action, now);
  Add(action, now);
  Drain();
  ASSERT_EQ(McapFiles().size(), 1u);

  // A directory in place of the next file: opening it fails for root too.
  const uint64_t split_ns = now + 1'000'000'000;
  const auto tm = ibmw::common::util::TimeT2TmLocal(static_cast<time_t>(split_ns / 1'000'000'000));
  char name[64];
  snprintf(name, sizeof(name), "ibmwbag_%04d%02d%02d_%02d%02d%02d.mcap", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  const fs::path blocker = McapFiles()[0].parent_path() / name;
  ASSERT_TRUE(fs::create_directory(blocker));
  Add(action, split_ns);
  Drain();
  fs::remove(blocker);
  EXPECT_EQ(McapFiles().size(), 1u);

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  Add(action, now + 5'000'000'000);
  Drain();
  EXPECT_EQ(McapFiles().size(), 2u);
  action.Shutdown();
}

// EXT-026: max_bag_size_m 0 means no limit, not a new file for every record.
TEST_F(RecordActionFreeSpaceTest, ZeroBagSizeNeverSplits) {
  RecordAction action;
  Initialize(action, "imd", "{max_bag_size_m: 0}");
  const uint64_t now = ibmw::common::util::GetCurTimestampNs();
  for (uint64_t i = 0; i < 3; ++i) Add(action, now + i * 2'000'000'000);
  Drain();
  EXPECT_EQ(McapFiles().size(), 1u);
  action.Shutdown();
}

// EXT-025: a signal record ends at its stop time on a silent topic too.
TEST_F(RecordActionFreeSpaceTest, SignalRecordEndsWithoutMessages) {
  RecordAction action;
  Initialize(action, "signal", "{msg_write_interval_time: 20}");
  std::string folder;
  ASSERT_TRUE(action.StartSignalRecord(0, 1, folder));
  ASSERT_FALSE(folder.empty());

  // A start while recording leaves the folder empty.
  std::string next_folder;
  ASSERT_TRUE(action.StartSignalRecord(0, 1, next_folder));
  EXPECT_TRUE(next_folder.empty());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (next_folder.empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(action.StartSignalRecord(0, 60, next_folder));
  }
  EXPECT_FALSE(next_folder.empty()) << "the record did not end";
  action.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
