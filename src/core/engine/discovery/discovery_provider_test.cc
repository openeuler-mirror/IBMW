// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/discovery_provider.h"

#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/runtime_graph/runtime_graph_agent.h"
#include "gtest/gtest.h"

namespace ibmw::discovery {
namespace {

class ScopedPluginDirectory {
 public:
  explicit ScopedPluginDirectory(const char* value) {
    if (const char* old = std::getenv("IBMW_DISCOVERY_PLUGIN_DIR")) previous_ = old;
    ::setenv("IBMW_DISCOVERY_PLUGIN_DIR", value, 1);
  }
  ~ScopedPluginDirectory() {
    if (previous_) ::setenv("IBMW_DISCOVERY_PLUGIN_DIR", previous_->c_str(), 1);
    else ::unsetenv("IBMW_DISCOVERY_PLUGIN_DIR");
  }
 private:
  std::optional<std::string> previous_;
};

using ibmw::runtime::core::runtime_graph::RuntimeGraphAgent;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

class DiscoveryProviderTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "ibmw-discovery-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    options_.registry_path = pattern;
  }
  void TearDown() override {
    agents_.clear();
    std::filesystem::remove_all(options_.registry_path);
  }
  void Register(std::string runtime, bool publisher, bool hidden = false) {
    TopicAdvertisement topic{
        .publisher_id = "endpoint",
        .topic = {.topic = hidden ? "/_hidden" : "/shared",
                  .msg_type = "example/Message", .serialization_type = "cdr",
                  .available_drivers = {"local", "dds"}},
        .published = publisher, .hidden = hidden,
        .driver_qos = {{"local", {}}, {"dds", {.policies = {{"reliability", "reliable"}}}}}};
    auto agent = std::make_unique<RuntimeGraphAgent>(RuntimeGraphAgent::Options{
        .registration = {.runtime_id = std::move(runtime), .topics = {topic}},
        .registry_path = options_.registry_path});
    agent->Initialize();
    agents_.push_back(std::move(agent));
  }

  DiscoveryOptions options_;
  std::vector<std::unique_ptr<RuntimeGraphAgent>> agents_;
};

TEST_F(DiscoveryProviderTest, ConvertsLiveRegistryEntriesWithoutInventingWireNames) {
  Register("runtime-b", false);
  Register("runtime-a", true);
  Register("runtime-c", true, true);
  const auto snapshot = Discover({options_});
  ASSERT_TRUE(snapshot.Succeeded());
  ASSERT_EQ(snapshot.sources.size(), 1u);
  EXPECT_TRUE(snapshot.sources[0].observed_at.has_value());
  ASSERT_EQ(snapshot.endpoints.size(), 3u);
  EXPECT_TRUE(snapshot.endpoints[0].hidden);
  const auto& publisher = snapshot.endpoints[1];
  EXPECT_EQ(publisher.source.backend, DiscoveryBackend::kRegistry);
  EXPECT_EQ(publisher.source.instance_id, options_.registry_path.string());
  EXPECT_TRUE(publisher.source.domain.empty());
  EXPECT_EQ(publisher.participant_id, "runtime-a");
  EXPECT_EQ(publisher.endpoint_id, "endpoint");
  EXPECT_EQ(publisher.role, EndpointRole::kPublisher);
  EXPECT_EQ(publisher.topic_name, "/shared");
  EXPECT_EQ(publisher.type_name, "example/Message");
  EXPECT_TRUE(publisher.wire_topic_name.empty());
  EXPECT_TRUE(publisher.wire_type_name.empty());
  EXPECT_EQ(publisher.serialization_type, "cdr");
  EXPECT_EQ(publisher.driver_qos.at("dds").policies.at("reliability"), "reliable");
  EXPECT_EQ(publisher.available_drivers, (std::vector<std::string>{"local", "dds"}));
  EXPECT_EQ(snapshot.endpoints[2].role, EndpointRole::kSubscriber);
  EXPECT_EQ(snapshot.endpoints[2].participant_id, "runtime-b");
}

TEST_F(DiscoveryProviderTest, ReusedProviderDropsExitedRuntimesAndCanRestart) {
  Register("runtime", true);
  auto provider = CreateDiscoveryProvider(DiscoveryBackend::kRegistry);
  ASSERT_NE(provider, nullptr);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  EXPECT_THROW(provider->Query(deadline), std::logic_error);
  provider->Start(options_);
  EXPECT_EQ(provider->Query(deadline).endpoints.size(), 1u);
  agents_.clear();
  EXPECT_TRUE(provider->Query(deadline).endpoints.empty());
  provider->Stop();
  provider->Stop();
  EXPECT_THROW(provider->Query(deadline), std::logic_error);
  EXPECT_NO_THROW(provider->Start(options_));
  EXPECT_TRUE(provider->Query(deadline).Succeeded());
  provider->Stop();
}

TEST_F(DiscoveryProviderTest, PreservesSuccessfulSourcesWhenOtherBackendsAreUnavailable) {
  ScopedPluginDirectory no_plugins("/dev/null");
  Register("runtime", true);
  const auto snapshot = Discover({
      {.backend = DiscoveryBackend::kDds, .domain_id = 10}, options_,
      {.backend = DiscoveryBackend::kRos2, .domain_id = 20},
      {.backend = DiscoveryBackend::kZenoh, .zenoh_domain = "robot_a"}});
  EXPECT_FALSE(snapshot.Succeeded());
  ASSERT_EQ(snapshot.sources.size(), 4u);
  ASSERT_EQ(snapshot.endpoints.size(), 1u);
  EXPECT_EQ(snapshot.sources[0].source.domain, "10");
  EXPECT_EQ(snapshot.sources[1].status, DiscoveryStatus::kReady);
  EXPECT_EQ(snapshot.sources[2].source.domain, "20");
  EXPECT_EQ(snapshot.sources[3].source.domain, "robot_a");
  for (const size_t index : {0u, 2u, 3u}) {
    EXPECT_EQ(snapshot.sources[index].status, DiscoveryStatus::kUnavailable);
    EXPECT_FALSE(snapshot.sources[index].observed_at.has_value());
    EXPECT_FALSE(snapshot.sources[index].message.empty());
  }
}

TEST_F(DiscoveryProviderTest, DistinguishesEmptyRegistryFromFailure) {
  const auto empty = Discover({options_});
  EXPECT_TRUE(empty.Succeeded());
  EXPECT_TRUE(empty.endpoints.empty());
  auto invalid = options_;
  invalid.registry_path /= std::string(5000, 'x');
  const auto failed = Discover({invalid});
  EXPECT_FALSE(failed.Succeeded());
  ASSERT_EQ(failed.sources.size(), 1u);
  EXPECT_EQ(failed.sources[0].status, DiscoveryStatus::kError);
  EXPECT_NE(failed.sources[0].message.find(invalid.registry_path.string()), std::string::npos);
}

TEST_F(DiscoveryProviderTest, KeepsDomainsAndObserverConfigurationsDistinct) {
  const auto dds = DescribeSource({.backend = DiscoveryBackend::kDds, .domain_id = 10});
  EXPECT_NE(dds, DescribeSource({.backend = DiscoveryBackend::kDds, .domain_id = 20}));
  EXPECT_NE(dds, DescribeSource({.backend = DiscoveryBackend::kRos2, .domain_id = 10}));
  EXPECT_NE(DescribeSource({.backend = DiscoveryBackend::kZenoh, .zenoh_config = "a.json5"}),
            DescribeSource({.backend = DiscoveryBackend::kZenoh, .zenoh_config = "b.json5"}));
}

// The enclosing constructor fails before its handler runs, destroying the
// provider member and unloading the plugin while the propagated error survives.
struct FailingOwner {
  std::unique_ptr<DiscoveryProvider> provider = CreateDiscoveryProvider(DiscoveryBackend::kDds);
  FailingOwner() {
    provider->Start({.backend = DiscoveryBackend::kDds});
    provider->Query(std::chrono::steady_clock::now());
  }
};

// COMM-033: an overridden plugin directory is only used when another user
// cannot have planted the library.
TEST(DiscoveryPluginDirectoryTest, OverrideRequiresTrustedDirectoryAndLibrary) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "ibmw-discovery-plugin-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto plugin = directory / "libibmw_discovery_dds.so";
  std::filesystem::copy_file(DISCOVERY_FAILURE_PLUGIN_PATH, plugin);
  ScopedPluginDirectory override_directory(directory.c_str());
  using std::filesystem::perms;
  const auto expect_rejected = [&] {
    try {
      CreateDiscoveryProvider(DiscoveryBackend::kDds);
      ADD_FAILURE() << "untrusted plugin was accepted";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("Untrusted discovery plugin"), std::string::npos)
          << error.what();
    }
    void* loaded = ::dlopen(plugin.c_str(), RTLD_NOW | RTLD_NOLOAD);
    EXPECT_EQ(loaded, nullptr);
    if (loaded) ::dlclose(loaded);
  };

  std::filesystem::permissions(plugin, perms::owner_all | perms::group_read | perms::group_exec |
                                           perms::others_read | perms::others_exec);
  std::filesystem::permissions(directory, perms::all);
  expect_rejected();
  std::filesystem::permissions(directory, perms::owner_all);
  std::filesystem::permissions(plugin, perms::others_write, std::filesystem::perm_options::add);
  expect_rejected();

  std::filesystem::permissions(plugin, perms::others_write, std::filesystem::perm_options::remove);
  EXPECT_NE(CreateDiscoveryProvider(DiscoveryBackend::kDds), nullptr);
  std::filesystem::remove_all(directory);
}

class DiscoveryPluginFailureTest : public testing::TestWithParam<const char*> {};

TEST_P(DiscoveryPluginFailureTest, ErrorSurvivesConstructorUnwindAndPluginUnload) {
  // A child also keeps each fixture environment and loader state independent.
  ASSERT_EXIT({
    ::setenv("IBMW_DISCOVERY_PLUGIN_DIR", DISCOVERY_FAILURE_PLUGIN_DIR, 1);
    ::setenv("IBMW_DISCOVERY_TEST_FAILURE", GetParam(), 1);
    bool diagnosed = false;
    try {
      FailingOwner owner;
    } catch (const std::runtime_error& error) {
      const std::string message = error.what();
      diagnosed = !message.empty();
      const std::string failure = GetParam();
      if (failure.ends_with("custom")) diagnosed = message == "plugin-owned diagnostic";
    } catch (...) {
      ::_exit(2);
    }
    void* still_loaded = ::dlopen(DISCOVERY_FAILURE_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (still_loaded) { ::dlclose(still_loaded); ::_exit(3); }
    ::_exit(diagnosed ? 0 : 4);
  }, testing::ExitedWithCode(0), "");
}

INSTANTIATE_TEST_SUITE_P(PluginBoundaries, DiscoveryPluginFailureTest,
    testing::Values("entry-custom", "entry-empty", "entry-nonstd",
                    "create-custom", "create-empty", "create-nonstd",
                    "start-custom", "start-empty", "start-nonstd",
                    "query-custom", "query-empty", "query-nonstd"));

}  // namespace
}  // namespace ibmw::discovery
