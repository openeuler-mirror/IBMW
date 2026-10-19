// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_options.h"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace ibmw::cli::topic {
namespace {

using ibmw::discovery::DiscoveryBackend;
using ibmw::discovery::DiscoveryOptions;

class DiscoveryOptionsTest : public testing::Test {
 protected:
  void SetUp() override {
    if (const char* value = std::getenv("ROS_DOMAIN_ID")) original_domain_ = value;
    ASSERT_EQ(::setenv("ROS_DOMAIN_ID", "17", 1), 0);
  }
  void TearDown() override {
    if (original_domain_) {
      ::setenv("ROS_DOMAIN_ID", original_domain_->c_str(), 1);
    } else {
      ::unsetenv("ROS_DOMAIN_ID");
    }
  }

  std::vector<DiscoveryOptions> Parse(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "discovery");
    ibmw::cli::Command command("discovery", "test");
    AddDiscoveryOptions(command);
    std::vector<DiscoveryOptions> result;
    command.SetHandler([&](const auto& invocation) {
      result = ParseDiscoveryOptions(invocation);
      return 0;
    });
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    EXPECT_EQ(command.Run(static_cast<int>(argv.size()), argv.data()), 0);
    return result;
  }

 private:
  std::optional<std::string> original_domain_;
};

TEST_F(DiscoveryOptionsTest, SupportsEverySourceCombinationAndDefaultsToRegistry) {
  const std::vector<std::string> flags{"--registry", "-d", "-r", "-z"};
  const std::vector<DiscoveryBackend> backends{
      DiscoveryBackend::kRegistry, DiscoveryBackend::kDds,
      DiscoveryBackend::kRos2, DiscoveryBackend::kZenoh};
  for (unsigned mask = 0; mask < 16; ++mask) {
    SCOPED_TRACE(mask);
    std::vector<std::string> arguments;
    std::vector<DiscoveryBackend> expected;
    for (size_t index = 0; index < flags.size(); ++index) {
      if ((mask & (1u << index)) != 0) {
        arguments.push_back(flags[index]);
        expected.push_back(backends[index]);
      }
    }
    if (expected.empty()) expected.push_back(DiscoveryBackend::kRegistry);
    const auto options = Parse(arguments);
    ASSERT_EQ(options.size(), expected.size());
    for (size_t index = 0; index < expected.size(); ++index) {
      EXPECT_EQ(options[index].backend, expected[index]);
    }
  }
  const auto all = Parse({"-a", "-d", "--dds", "--registry"});
  ASSERT_EQ(all.size(), 4u);
  for (size_t index = 0; index < all.size(); ++index) {
    EXPECT_EQ(all[index].backend, backends[index]);
  }
}

TEST_F(DiscoveryOptionsTest, ResolvesDomainsWithoutChangingProcessEnvironment) {
  const auto defaults = Parse({"--dds", "--ros2"});
  ASSERT_EQ(defaults.size(), 2u);
  EXPECT_EQ(defaults[0].domain_id, 0u);
  EXPECT_EQ(defaults[1].domain_id, 17u);
  const auto shared = Parse({"-d", "-r", "--domain=10"});
  EXPECT_EQ(shared[0].domain_id, 10u);
  EXPECT_EQ(shared[1].domain_id, 10u);
  const auto overrides = Parse({"-d", "-r", "--domain", "10",
                                 "--dds-domain", "20", "--ros-domain", "0"});
  EXPECT_EQ(overrides[0].domain_id, 20u);
  EXPECT_EQ(overrides[1].domain_id, 0u);
  EXPECT_STREQ(std::getenv("ROS_DOMAIN_ID"), "17");
  ASSERT_EQ(::unsetenv("ROS_DOMAIN_ID"), 0);
  EXPECT_EQ(Parse({"-r"}).front().domain_id, 0u);
}

TEST_F(DiscoveryOptionsTest, OnlyReadsRosEnvironmentWhenNeeded) {
  ASSERT_EQ(::setenv("ROS_DOMAIN_ID", "invalid", 1), 0);
  EXPECT_NO_THROW(Parse({}));
  EXPECT_NO_THROW(Parse({"-d"}));
  EXPECT_NO_THROW(Parse({"-z"}));
  EXPECT_THROW(Parse({"-r"}), std::invalid_argument);
  EXPECT_EQ(Parse({"-r", "--domain", "12"}).front().domain_id, 12u);
  EXPECT_EQ(Parse({"-r", "--ros-domain", "13"}).front().domain_id, 13u);
}

TEST_F(DiscoveryOptionsTest, KeepsZenohDomainIndependentOfNumericDomains) {
  const auto options = Parse({"--all", "--domain", "10",
                              "--zenoh-domain", "robot/a",
                              "--zenoh-config", "/tmp/zenoh.json5",
                              "--registry-path", "/tmp/custom-registry"});
  ASSERT_EQ(options.size(), 4u);
  EXPECT_EQ(options[0].registry_path, "/tmp/custom-registry");
  EXPECT_EQ(options[1].domain_id, 10u);
  EXPECT_EQ(options[2].domain_id, 10u);
  EXPECT_EQ(options[3].zenoh_domain, "robot/a");
  EXPECT_EQ(options[3].zenoh_config, "/tmp/zenoh.json5");
  EXPECT_TRUE(Parse({"-z"}).front().zenoh_domain.empty());
}

TEST_F(DiscoveryOptionsTest, RejectsInvalidAndRepeatedDomainValues) {
  for (const std::string option : {"--domain", "--dds-domain", "--ros-domain"}) {
    for (const std::string value : {"-1", "+1", "10x", "1.5", "4294967296", " 10", ""}) {
      SCOPED_TRACE(option + "=" + value);
      const std::vector<std::string> arguments = value.empty()
          ? std::vector<std::string>{"-d", "-r", option, ""}
          : std::vector<std::string>{"-d", "-r", option + "=" + value};
      EXPECT_THROW(Parse(arguments), std::invalid_argument);
    }
  }
  EXPECT_THROW(Parse({"-d", "--domain", "1", "--domain", "2"}), std::invalid_argument);
  EXPECT_THROW(Parse({"-r", "--ros-domain", "1", "--ros-domain", "2"}), std::invalid_argument);
  EXPECT_THROW(Parse({"-z", "--zenoh-domain", "a", "--zenoh-domain", "b"}), std::invalid_argument);
}

TEST_F(DiscoveryOptionsTest, RejectsOptionsForUnselectedSources) {
  const std::vector<std::vector<std::string>> cases{
      {"--domain", "10"}, {"-z", "--domain", "10"},
      {"-r", "--dds-domain", "10"}, {"-d", "--ros-domain", "10"},
      {"-d", "--zenoh-domain", "robot"}, {"-r", "--zenoh-config", "zenoh.json5"},
      {"-d", "--registry-path", "/tmp/registry"},
      {"--registry-path", ""}, {"-z", "--zenoh-config", ""}};
  for (const auto& arguments : cases) {
    SCOPED_TRACE(testing::PrintToString(arguments));
    EXPECT_THROW(Parse(arguments), std::invalid_argument);
  }
}

}  // namespace
}  // namespace ibmw::cli::topic
