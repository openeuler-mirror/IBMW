// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for EXT-021: optional disk usage limits of the recorder
// (min_free_space_m, max_folder_num, max_total_size_m), all off by default.

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

  Record(bags, "{max_folder_num: 2}");

  const auto names = List(bags);
  ASSERT_EQ(names.size(), 5u) << ::testing::PrintToString(names);
  EXPECT_EQ(names[0], "ibmwbag_19990101_000000");  // the link
  EXPECT_EQ(names[1], "ibmwbag_20200103_000000");  // newest old folder
  EXPECT_EQ(names[2], "ibmwbag_2020010_0000000");
  EXPECT_NE(names[3], "ibmwbag_20200101_000000");  // the new folder
  EXPECT_EQ(names[4], "my_ibmwbag_20200101_000000");
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

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
