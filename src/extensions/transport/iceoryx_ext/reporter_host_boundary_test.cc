#include "extension_api/ibmw_reporter_v1.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <gtest/gtest.h>

#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace {
using Query = const ibmw_reporter_v1* (*)(void);

class ReporterHostBoundary : public ::testing::Test {
 protected:
  void TearDown() override {
    unsetenv("IBMW_CORE_METRICS");
    unsetenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
#ifdef IBMW_ENABLE_CORE_METRICS
    ibmw::runtime::core::observability::Controller::Instance().ResetForTesting();
#endif
  }
};

TEST_F(ReporterHostBoundary, DeepbindIceoryxResolvesTheHostOwner) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
#ifdef IBMW_ENABLE_CORE_METRICS
  ibmw::runtime::core::observability::Controller::Instance().Initialize();
#endif
  void* plugin = dlopen(ICEORYX_EXTENSION_PATH, RTLD_NOW | RTLD_LOCAL
#ifdef RTLD_DEEPBIND
                        | RTLD_DEEPBIND
#endif
  );
  ASSERT_NE(plugin, nullptr) << dlerror();
  auto plugin_query = reinterpret_cast<Query>(
      dlsym(plugin, "ibmw_iceoryx_query_host_reporter_v1"));
  ASSERT_NE(plugin_query, nullptr) << dlerror();
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(host, nullptr);
  auto host_query = reinterpret_cast<Query>(dlsym(host, "ibmw_reporter_query_v1"));
  ASSERT_NE(host_query, nullptr);

  Dl_info owner{};
  ASSERT_NE(dladdr(reinterpret_cast<void*>(host_query), &owner), 0);
  ASSERT_NE(owner.dli_fname, nullptr);
  EXPECT_EQ(std::strstr(owner.dli_fname, "ibmw_iceoryx_transport"), nullptr);
#ifdef IBMW_ENABLE_REPORTER_V1
  const auto* api = plugin_query();
  ASSERT_EQ(api, host_query());
  ASSERT_NE(api, nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"fixture", 7};
  descriptor.metric = {"count", 5};
  auto* slot = api->register_slot(api->owner, &descriptor);
  ASSERT_NE(slot, nullptr);
  api->add(api->owner, slot, 1);
#else
  EXPECT_EQ(plugin_query(), nullptr);
  EXPECT_EQ(host_query(), nullptr);
#endif
  dlclose(host);
  dlclose(plugin);
}

TEST_F(ReporterHostBoundary, RuntimeOffIsSafeNull) {
#ifdef IBMW_ENABLE_CORE_METRICS
  ibmw::runtime::core::observability::Controller::Instance().Initialize();
#endif
  EXPECT_EQ(ibmw_reporter_query_v1(), nullptr);
}

#if defined(IBMW_ENABLE_CORE_METRICS) && defined(IBMW_ENABLE_REPORTER_V1)
TEST_F(ReporterHostBoundary, QueryTracksControllerLifecycle) {
  const auto dir = std::filesystem::temp_directory_path() /
                   "ibmw_reporter_lifecycle_test";
  std::filesystem::remove_all(dir);
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  ASSERT_EQ(setenv("IBMW_OBSERVABILITY_OUTPUT_DIR", dir.c_str(), 1), 0);
  auto& controller =
      ibmw::runtime::core::observability::Controller::Instance();

  EXPECT_EQ(ibmw_reporter_query_v1(), nullptr);
  controller.Initialize();
  EXPECT_NE(ibmw_reporter_query_v1(), nullptr);
  controller.Start();
  EXPECT_NE(ibmw_reporter_query_v1(), nullptr);
  EXPECT_TRUE(controller.FinalizeSnapshot());
  EXPECT_EQ(ibmw_reporter_query_v1(), nullptr);

  unsetenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
  std::filesystem::remove_all(dir);
}

TEST_F(ReporterHostBoundary, RetiredSlotsRemainSafeAndGenerationIsolated) {
  const auto dir = std::filesystem::temp_directory_path() /
                   "ibmw_reporter_stale_slot_test";
  std::filesystem::remove_all(dir);
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  ASSERT_EQ(setenv("IBMW_OBSERVABILITY_OUTPUT_DIR", dir.c_str(), 1), 0);
  auto& controller =
      ibmw::runtime::core::observability::Controller::Instance();

  controller.Initialize();
  const auto* first_api = ibmw_reporter_query_v1();
  ASSERT_NE(first_api, nullptr);
  ibmw_reporter_slot_descriptor_v1 old_descriptor{};
  old_descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  old_descriptor.struct_size = sizeof(old_descriptor);
  old_descriptor.kind = {"retired_generation", 18};
  old_descriptor.metric = {"old_count", 9};
  auto* old_slot =
      first_api->register_slot(first_api->owner, &old_descriptor);
  ASSERT_NE(old_slot, nullptr);
  first_api->add(first_api->owner, old_slot, 1);
  controller.Start();
  ASSERT_TRUE(controller.FinalizeSnapshot());

  // The V1 API has no unregister operation, so an extension may add late.
  first_api->add(first_api->owner, old_slot, 2);

  controller.Initialize();
  const auto* second_api = ibmw_reporter_query_v1();
  ASSERT_NE(second_api, nullptr);
  ibmw_reporter_slot_descriptor_v1 new_descriptor{};
  new_descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  new_descriptor.struct_size = sizeof(new_descriptor);
  new_descriptor.kind = {"current_generation", 18};
  new_descriptor.metric = {"new_count", 9};
  auto* new_slot =
      second_api->register_slot(second_api->owner, &new_descriptor);
  ASSERT_NE(new_slot, nullptr);
  EXPECT_NE(new_slot, old_slot);

  first_api->add(first_api->owner, old_slot, 4);
  second_api->add(second_api->owner, new_slot, 8);
  const auto snapshot = controller.registry().ReporterSnapshotJsonl();
  EXPECT_EQ(snapshot.find("retired_generation"), std::string::npos);
  EXPECT_EQ(snapshot.find("old_count"), std::string::npos);
  EXPECT_NE(snapshot.find("current_generation"), std::string::npos);
  EXPECT_NE(snapshot.find("\"metric\":\"new_count\""), std::string::npos);
  EXPECT_NE(snapshot.find("\"value\":8"), std::string::npos);

  controller.Start();
  EXPECT_TRUE(controller.FinalizeSnapshot());
  std::filesystem::remove_all(dir);
}
#endif
}  // namespace
