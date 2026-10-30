// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for EXT-021: optional disk usage limits of the recorder
// (min_free_space_m, max_folder_num, max_total_size_m), all off by default.

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
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

  void Initialize(RecordAction& action, const std::string& mode, const std::string& storage_policy) {
    action.RegisterGetExecutorFunc([this](std::string_view) { return Handle(); });
    action.RegisterGetTypeSupportFunc([](std::string_view) { return ibmw::util::TypeSupportRef(); });
    action.RegisterGetSchemaSupportFunc(
        [](std::string_view, std::string_view) -> std::optional<ibmw::util::SchemaDescriptor> {
          return std::nullopt;
        });
    action.SetFreeSpaceQuery([this](const fs::path&) { return free_space_.load(); });
    YAML::Node options = YAML::Load("{executor: storage-policy-test, topic_meta_list: []}");
    options["mode"] = mode;
    options["bag_path"] = (root_ / "bags").string();
    options["storage_policy"] = YAML::Load(storage_policy);
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

  void Add(RecordAction& action, uint64_t log_timestamp) {
    action.AddRecord({.topic_index = 1,
                      .log_timestamp = log_timestamp,
                      .pub_timestamp = log_timestamp,
                      .pub_sequence = 0,
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

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
