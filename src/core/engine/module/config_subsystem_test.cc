// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests (CORE-019): per-module temp config files must stay inside
// temp_cfg_path, be private to the owner and never follow planted symlinks.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "engine/module/config_subsystem.h"

namespace ibmw::runtime::core::configurator {
namespace {

namespace fs = std::filesystem;

class ConfigSubsystemTempCfgTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("ibmw_cfg_test_" + std::to_string(::getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_ / "tmp");
    cfg_.SetLogger(std::make_shared<ibmw::common::util::LoggerWrapper>());
    YAML::Node root;
    root["ibmw"]["config"]["temp_cfg_path"] = (dir_ / "tmp").string();
    root["good_mod"]["key"] = 1;
    root["/../../evil"]["key"] = 2;
    cfg_.Initialize(root);
  }
  void TearDown() override { fs::remove_all(dir_); }

  static util::ModuleDetailInfo Info(const std::string& name) {
    util::ModuleDetailInfo info;
    info.module_name = name;
    return info;
  }

  static std::string ReadAll(const fs::path& p) {
    std::ifstream ifs(p);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
  }

  fs::path dir_;
  ConfigSubsystem cfg_;
};

TEST_F(ConfigSubsystemTempCfgTest, RejectsModuleNameWithPathSeparators) {
  // tmp/temp_cfg_file_for_/../../evil.yaml resolves to <dir>/evil.yaml once the
  // prefix directory exists, i.e. outside temp_cfg_path.
  fs::create_directories(dir_ / "tmp" / "temp_cfg_file_for_");
  EXPECT_ANY_THROW(cfg_.GetConfiguratorAccess(Info("/../../evil")));
  EXPECT_FALSE(fs::exists(dir_ / "evil.yaml"));
}

TEST_F(ConfigSubsystemTempCfgTest, AcceptsNonAsciiModuleNameWithoutSeparators) {
  YAML::Node root;
  root["ibmw"]["config"]["temp_cfg_path"] = (dir_ / "tmp").string();
  root["mod:v2 \xc3\xa9"]["key"] = 3;
  ConfigSubsystem cfg;
  cfg.SetLogger(std::make_shared<ibmw::common::util::LoggerWrapper>());
  cfg.Initialize(root);
  EXPECT_NO_THROW(cfg.GetConfiguratorAccess(Info("mod:v2 \xc3\xa9")));
  EXPECT_TRUE(fs::exists(dir_ / "tmp" / "temp_cfg_file_for_mod:v2 \xc3\xa9.yaml"));
}

TEST_F(ConfigSubsystemTempCfgTest, WritesOwnerOnlyFile) {
  const mode_t old_mask = ::umask(022);
  cfg_.GetConfiguratorAccess(Info("good_mod"));
  ::umask(old_mask);
  const auto file = dir_ / "tmp" / "temp_cfg_file_for_good_mod.yaml";
  struct stat st {};
  ASSERT_EQ(::lstat(file.c_str(), &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  EXPECT_NE(ReadAll(file).find("key: 1"), std::string::npos);
}

TEST_F(ConfigSubsystemTempCfgTest, DoesNotFollowPlantedSymlink) {
  const auto victim = dir_ / "victim.txt";
  { std::ofstream(victim) << "precious"; }
  const auto file = dir_ / "tmp" / "temp_cfg_file_for_good_mod.yaml";
  fs::create_symlink(victim, file);

  cfg_.GetConfiguratorAccess(Info("good_mod"));

  EXPECT_EQ(ReadAll(victim), "precious");
  EXPECT_FALSE(fs::is_symlink(file));
  EXPECT_NE(ReadAll(file).find("key: 1"), std::string::npos);
}

}  // namespace
}  // namespace ibmw::runtime::core::configurator
