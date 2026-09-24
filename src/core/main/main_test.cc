// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "gtest/gtest.h"
#include "yaml-cpp/yaml.h"

int RunCli(int argc, char** argv);

namespace {

class CliTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "ibmw-cli-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  int Run(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return RunCli(static_cast<int>(argv.size()), argv.data());
  }

  std::filesystem::path WriteProviderConfig() {
    const auto path = directory_ / "cli config.yaml";
    YAML::Node root;
    root["providers"][0]["library"] = IBMW_TEST_CLI_PROVIDER;
    std::ofstream(path) << root;
    return path;
  }

  std::filesystem::path directory_;
};

TEST_F(CliTest, ProviderReceivesArgumentsThroughCliEntry) {
  const auto config = WriteProviderConfig();
  for (const auto& command : {std::string("inspect"), std::string("bag"),
                              std::string("topic")}) {
    EXPECT_EQ(Run({"ibmw", "--cli-config", config.string(), command, "--help"}), 17);
    std::vector<std::string> args{"ibmw", "--cli-config", config.string(), command};
    if (command == "bag") args.push_back("record");
    args.insert(args.end(), {"--help", "two words"});
    EXPECT_EQ(Run(std::move(args)), 17);
  }
  EXPECT_EQ(Run({"ibmw", "--cli-config=" + config.string(), "inspect",
                 "--help", "two words"}), 17);
}

TEST_F(CliTest, BuiltinHelpAndVersionDoNotRequireProviders) {
  const auto config = (directory_ / "missing.yaml").string();
  for (const auto& command : {"--help", "version", "--version", "-v"}) {
    testing::internal::CaptureStdout();
    const auto result = Run({"ibmw", "--cli-config", config, command});
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(result, 0);
    EXPECT_FALSE(output.empty());
    EXPECT_EQ(output.find("bag"), std::string::npos);
    EXPECT_EQ(output.find("topic"), std::string::npos);
  }
}

TEST_F(CliTest, LoadsProviderFromDefaultConfiguration) {
  const char* configured = std::getenv("IBMW_CLI_CONFIG");
  const bool had_configuration = configured != nullptr;
  const std::string previous_configuration = configured ? configured : "";
  ASSERT_EQ(::setenv("IBMW_CLI_CONFIG", WriteProviderConfig().c_str(), 1), 0);
  EXPECT_EQ(Run({"ibmw", "inspect", "--help", "two words"}), 17);
  if (had_configuration) {
    EXPECT_EQ(::setenv("IBMW_CLI_CONFIG", previous_configuration.c_str(), 1), 0);
  } else {
    EXPECT_EQ(::unsetenv("IBMW_CLI_CONFIG"), 0);
  }
}

}  // namespace
