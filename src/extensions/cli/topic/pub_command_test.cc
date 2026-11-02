// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "observation_test_support.h"
#include "pub_options.h"
#include <chrono>
#include <limits>
#include <utility>

namespace ibmw::cli::topic {
namespace {
using test::Invoke;

TEST(PubCommandTest, RejectsInvalidOptionsBeforeCreatingPublisher) {
  for (const auto& options : std::vector<std::vector<std::string>>{
      {"--rate", "nan"}, {"--rate", "0"}, {"--rate", "inf"},
      {"--times", "0"}, {"--times", "-1"}, {"--times", "1.5"},
      {"--once", "--times", "2"}, {"--wait", "-1"}, {"--keep-alive", "nan"},
      {"--dds"}, {"--domain", "1"}}) {
    std::vector<std::string> args{"pub", "/pub", "json:PublicationFixture", "{value: 42}", "--local"};
    args.insert(args.end(), options.begin(), options.end());
    const auto result = Invoke(args);
    EXPECT_EQ(result.code, 1) << testing::PrintToString(options);
    EXPECT_EQ(result.error.find("Publishing "), std::string::npos);
  }
}

TEST(PubCommandTest, RejectsWrongTypeAndPayloadBeforeCreatingPublisher) {
  for (const auto& payload : {"{value: bad}", "value: 1\n---\nvalue: 2"}) {
    const auto result = Invoke({"pub", "/pub", "json:PublicationFixture", payload, "--local",
        "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once", "--wait", "0", "--keep-alive", "0"});
    EXPECT_EQ(result.code, 1);
    EXPECT_EQ(result.error.find("Publishing "), std::string::npos);
  }
  const auto result = Invoke({"pub", "/pub", "json:Unknown", "{}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once"});
  EXPECT_EQ(result.code, 1);
  EXPECT_NE(result.error.find("No TypeSupport"), std::string::npos);
}

TEST(PubCommandTest, PublishesRequestedCountAtBoundedRateAndCanRestart) {
  const auto start = std::chrono::steady_clock::now();
  const auto repeated = Invoke({"pub", "/pub", "json:PublicationFixture", "{value: 42}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--times", "3", "--rate", "20",
      "--wait", "0", "--keep-alive", "0"});
  EXPECT_EQ(repeated.code, 0) << repeated.error;
  EXPECT_NE(repeated.error.find("Published 3 message(s)."), std::string::npos);
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(90));
  const auto once = Invoke({"pub", "/pub", "json:PublicationFixture", "{value: 7}", "--local",
      "--type-support", PUB_TYPE_SUPPORT_FIXTURE_PATH, "--once", "--wait", "0", "--keep-alive", "0"});
  EXPECT_EQ(once.code, 0) << once.error;
  EXPECT_NE(once.error.find("Published 1 message(s)."), std::string::npos);
}

// Builds the Engine configuration only: no transport library is loaded.
PublicationConfig Parse(const std::vector<std::pair<std::string, std::string>>& options) {
  Invocation invocation;
  invocation.positional_args = {"/pub", "json:PublicationFixture", "{value: 42}"};
  for (const auto& [name, value] : options) invocation.options[name].push_back(value);
  return ParsePublicationOptions(invocation);
}

TEST(PubCommandTest, IceoryxAndShmDriversMapToTheirExtensionOptions) {
  const auto iceoryx = Parse({{"--iceoryx", ""}, {"--iceoryx-runtime-id", "rt"}, {"--shm-init-size", "4096"}});
  EXPECT_EQ(iceoryx.driver, "iceoryx");
  auto ext = iceoryx.root["ibmw"]["extensions"]["items"][0];
  EXPECT_EQ(ext["name"].as<std::string>(), "iceoryx_extension");
  EXPECT_NE(ext["path"].as<std::string>().find("libibmw_iceoryx_transport.so"), std::string::npos);
  EXPECT_EQ(ext["options"]["runtime_id"].as<std::string>(), "rt");
  EXPECT_EQ(ext["options"]["shm_init_size"].as<uint64_t>(), 4096u);
  EXPECT_FALSE(iceoryx.root["ibmw"]["messaging"]["drivers"][0]["options"]["publishers"].IsDefined());

  const auto shm = Parse({{"--shm", ""}});
  EXPECT_EQ(shm.driver, "shm");
  ext = shm.root["ibmw"]["extensions"]["items"][0];
  EXPECT_EQ(ext["name"].as<std::string>(), "shm_extension");
  EXPECT_NE(ext["path"].as<std::string>().find("libibmw_shm_transport.so"), std::string::npos);
  EXPECT_EQ(ext["options"]["runtime_id"].as<std::string>().rfind("ibmw_topic_pub_", 0), 0u);
  EXPECT_EQ(ext["options"]["shm_init_size"].as<uint64_t>(), 1024u);
  for (const auto* key : {"domain", "root_dir", "group", "max_bytes_per_publisher", "prefault", "require_read_only"})
    EXPECT_FALSE(ext["options"][key].IsDefined()) << key;
  const auto messaging = shm.root["ibmw"]["messaging"];
  EXPECT_EQ(messaging["drivers"][0]["type"].as<std::string>(), "shm");
  EXPECT_FALSE(messaging["drivers"][0]["options"]["publishers"].IsDefined());
  EXPECT_EQ(messaging["publishers"][0]["enable_drivers"][0].as<std::string>(), "shm");

  const auto full = Parse({{"--shm", ""}, {"--shm-runtime-id", "cli_rt"}, {"--shm-init-size", "8192"},
                           {"--shm-domain", "lab"}, {"--shm-root-dir", "/tmp/shm_root"}, {"--shm-group", "4294967294"},
                           {"--shm-max-bytes-per-publisher", "1048576"}, {"--shm-prefault", ""},
                           {"--shm-require-read-only", ""}, {"--transport-library", "/opt/libshm.so"}});
  ext = full.root["ibmw"]["extensions"]["items"][0];
  EXPECT_EQ(ext["path"].as<std::string>(), "/opt/libshm.so");
  const auto settings = ext["options"];
  EXPECT_EQ(settings["runtime_id"].as<std::string>(), "cli_rt");
  EXPECT_EQ(settings["shm_init_size"].as<uint64_t>(), 8192u);
  EXPECT_EQ(settings["domain"].as<std::string>(), "lab");
  EXPECT_EQ(settings["root_dir"].as<std::string>(), "/tmp/shm_root");
  EXPECT_EQ(settings["group"].as<std::string>(), "4294967294");
  EXPECT_EQ(settings["max_bytes_per_publisher"].as<uint64_t>(), 1048576u);
  EXPECT_TRUE(settings["prefault"].as<bool>());
  EXPECT_TRUE(settings["require_read_only"].as<bool>());
}

TEST(PubCommandTest, RejectsShmOptionsForOtherDriversAndInvalidValues) {
  for (const auto& [option, value] : std::vector<std::pair<std::string, std::string>>{
           {"--shm-runtime-id", "x"}, {"--shm-domain", "d"}, {"--shm-root-dir", "/tmp"}, {"--shm-group", "1"},
           {"--shm-max-bytes-per-publisher", "1"}, {"--shm-prefault", ""}, {"--shm-require-read-only", ""}}) {
    EXPECT_THROW(Parse({{"--iceoryx", ""}, {option, value}}), std::invalid_argument) << option;
    EXPECT_THROW(Parse({{"--local", ""}, {option, value}}), std::invalid_argument) << option;
  }
  EXPECT_THROW(Parse({{"--shm", ""}, {"--iceoryx-runtime-id", "x"}}), std::invalid_argument);
  EXPECT_THROW(Parse({{"--local", ""}, {"--shm-init-size", "1024"}}), std::invalid_argument);
  EXPECT_THROW(Parse({{"--shm", ""}, {"--iceoryx", ""}}), std::invalid_argument);
  for (const auto& [option, value] : std::vector<std::pair<std::string, std::string>>{
           {"--shm-init-size", "0"}, {"--shm-init-size", "4294967296"}, {"--shm-init-size", "1k"},
           {"--shm-runtime-id", ""}, {"--shm-domain", ""}, {"--shm-root-dir", ""},
           {"--shm-group", "4294967295"}, {"--shm-group", "-1"}, {"--shm-group", "staff"}, {"--shm-group", ""},
           {"--shm-max-bytes-per-publisher", "0"}, {"--shm-max-bytes-per-publisher", "x"}}) {
    EXPECT_THROW(Parse({{"--shm", ""}, {option, value}}), std::invalid_argument) << option << "=" << value;
  }
  EXPECT_THROW(Parse({{"--shm", ""}, {"--shm-domain", "a"}, {"--shm-domain", "b"}}), std::invalid_argument);
  EXPECT_EQ(Parse({{"--shm", ""}, {"--shm-group", "0"}}).root["ibmw"]["extensions"]["items"][0]["options"]["group"]
                .as<std::string>(),
            "0");
}
}  // namespace
}  // namespace ibmw::cli::topic
