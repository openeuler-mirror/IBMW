// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for EXT-022: copying extra_file_path into a new bag folder
// does not follow symbolic links, is size bounded and copies every entry.

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "record_playback_ext/record_action.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

namespace fs = std::filesystem;

class RecordActionExtraFileTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("ibmw_extra_file_test_" + std::to_string(::getpid()));
    fs::remove_all(root_);
    fs::create_directories(root_ / "src");
    fs::create_directories(root_ / "bags");
  }

  void TearDown() override { fs::remove_all(root_); }

  static void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
  }

  // Creates a bag folder (imd mode copies the extra files synchronously)
  // and returns its extra_file folder.
  fs::path Record(const std::vector<std::string>& extra_file_path) {
    YAML::Node options = YAML::Load("{mode: imd, executor: none, topic_meta_list: []}");
    options["bag_path"] = (root_ / "bags").string();
    for (const auto& path : extra_file_path) options["extra_file_path"].push_back(path);

    RecordAction action;
    action.RegisterGetExecutorFunc([](std::string_view) { return ibmw::executor::ExecutorHandle(); });
    action.RegisterGetTypeSupportFunc([](std::string_view) { return ibmw::util::TypeSupportRef(); });
    action.RegisterGetSchemaSupportFunc(
        [](std::string_view, std::string_view) -> std::optional<ibmw::util::SchemaDescriptor> {
          return std::nullopt;
        });
    action.Initialize(options);
    action.Shutdown();

    for (const auto& bag : fs::directory_iterator(root_ / "bags")) return bag.path() / "extra_file";
    ADD_FAILURE() << "no bag folder was created";
    return {};
  }

  fs::path root_;
};

TEST_F(RecordActionExtraFileTest, CopiesEveryEntryAndSkipsSymbolicLinks) {
  WriteFile(root_ / "src/a.yaml", "a");
  WriteFile(root_ / "src/conf/b.yaml", "b");
  WriteFile(root_ / "src/conf/sub/c.yaml", "c");
  WriteFile(root_ / "outside/secret", "s");
  fs::create_directory_symlink(root_ / "outside", root_ / "src/conf/link_dir");
  fs::create_symlink(root_ / "outside/secret", root_ / "src/conf/link_file");
  fs::create_directory_symlink(root_ / "src/conf", root_ / "src/conf/sub/loop");

  // A trailing separator must not make the directory replace the folder.
  const auto extra = Record({(root_ / "src/a.yaml").string(), (root_ / "src/conf").string() + "/"});

  EXPECT_TRUE(fs::is_regular_file(extra / "a.yaml"));
  EXPECT_TRUE(fs::is_regular_file(extra / "conf/b.yaml"));
  EXPECT_TRUE(fs::is_regular_file(extra / "conf/sub/c.yaml"));
  EXPECT_FALSE(fs::exists(fs::symlink_status(extra / "conf/link_dir")));
  EXPECT_FALSE(fs::exists(fs::symlink_status(extra / "conf/link_file")));
  EXPECT_FALSE(fs::exists(fs::symlink_status(extra / "conf/sub/loop")));
}

// The copy keeps the configured name, also when the configured path is a link.
TEST_F(RecordActionExtraFileTest, ConfiguredLinkKeepsItsName) {
  WriteFile(root_ / "src/v3.yaml", "v3");
  fs::create_symlink(root_ / "src/v3.yaml", root_ / "src/current.yaml");
  WriteFile(root_ / "src/real_dir/x.yaml", "x");
  fs::create_directory_symlink(root_ / "src/real_dir", root_ / "src/conf_dir");

  const auto extra = Record({(root_ / "src/current.yaml").string(), (root_ / "src/conf_dir").string() + "/"});

  EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(extra / "current.yaml")));
  EXPECT_FALSE(fs::exists(extra / "v3.yaml"));
  EXPECT_TRUE(fs::is_regular_file(extra / "conf_dir/x.yaml"));
  EXPECT_FALSE(fs::exists(extra / "real_dir"));
}

TEST_F(RecordActionExtraFileTest, DirectoryWithTooManyEntriesIsSkipped) {
  WriteFile(root_ / "src/small.yaml", "small");
  fs::create_directories(root_ / "src/many");
  for (int i = 0; i <= 10000; ++i) std::ofstream(root_ / "src/many" / std::to_string(i));

  const auto extra = Record({(root_ / "src/many").string(), (root_ / "src/small.yaml").string()});

  EXPECT_FALSE(fs::exists(extra / "many"));
  EXPECT_TRUE(fs::is_regular_file(extra / "small.yaml"));
}

TEST_F(RecordActionExtraFileTest, OversizedEntryIsSkipped) {
  WriteFile(root_ / "src/small.yaml", "small");
  {
    // Sparse: takes no disk space.
    std::ofstream(root_ / "src/huge.bin");
    fs::resize_file(root_ / "src/huge.bin", (256ULL << 20) + 1);
  }
  WriteFile(root_ / "src/dir/x", "x");
  fs::resize_file(root_ / "src/dir/x", (256ULL << 20) + 1);

  const auto extra = Record({(root_ / "src/huge.bin").string(), (root_ / "src/dir").string(),
                             (root_ / "src/small.yaml").string()});

  EXPECT_FALSE(fs::exists(extra / "huge.bin"));
  EXPECT_FALSE(fs::exists(extra / "dir"));
  EXPECT_TRUE(fs::is_regular_file(extra / "small.yaml"));
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
