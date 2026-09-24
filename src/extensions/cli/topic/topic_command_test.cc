// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <string>
#include <vector>

#include "extension_api/ibmw_cli_provider.h"
#include "gtest/gtest.h"

namespace {

TEST(TopicCommandTest, ReportsDiscoveryFailureAtProviderBoundary) {
  const auto entry = IbmwCliEntryV1();
  ASSERT_EQ(entry.command_count, 1u);
  const std::string invalid_path = "/tmp/" + std::string(5000, 'x');
  for (const std::string command : {"list", "type", "find", "info"}) {
    SCOPED_TRACE(command);
    std::vector<std::string> arguments{"topic", command};
    if (command != "list") arguments.push_back("example");
    arguments.insert(arguments.end(), {"--registry-path", invalid_path});
    std::vector<const char*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());

    testing::internal::CaptureStderr();
    int result = 0;
    EXPECT_NO_THROW(result = entry.commands[0].invoke(
                        static_cast<int>(argv.size()), argv.data()));
    const auto error = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, 1);
    EXPECT_NE(error.find("ibmw topic: error:"), std::string::npos);
    EXPECT_NE(error.find(invalid_path), std::string::npos);
  }
}

}  // namespace
