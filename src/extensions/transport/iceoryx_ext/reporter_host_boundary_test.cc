#include "extension_api/ibmw_reporter_v1.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <array>
#include <atomic>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif
#ifdef IBMW_ENABLE_REPORTER_V1
#include "internal/reporter_sidecar.h"
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
  EXPECT_EQ(ibmw_reporter_query_batch_v1(), nullptr);
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
  EXPECT_NE(ibmw_reporter_query_batch_v1(), nullptr);
  controller.Start();
  EXPECT_NE(ibmw_reporter_query_v1(), nullptr);
  EXPECT_TRUE(controller.FinalizeSnapshot());
  EXPECT_EQ(ibmw_reporter_query_v1(), nullptr);
  EXPECT_EQ(ibmw_reporter_query_batch_v1(), nullptr);

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

TEST_F(ReporterHostBoundary,
       RetainedReceiveSnapshotsAllMetricsAndCallbackConservation) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller =
      ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* api = ibmw_reporter_query_v1();
  ASSERT_NE(api, nullptr);

  ibmw::extensions::iceoryx_extension::IoxReceiveReporter reporter;
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(batch, nullptr);
  ASSERT_TRUE(ibmw::extensions::iceoryx_extension::RegisterIoxReceiveReporterSlots(
      api, batch, reporter, "retained/topic", "RetainedType"));

  constexpr uint64_t kEntered = 4;
  constexpr uint64_t kCompleted = 2;
  constexpr uint64_t kThrewStd = 1;
  constexpr uint64_t kThrewNonstd = 1;
  api->add(api->owner, reporter.delivered, 4);
  api->add(api->owner, reporter.callback_entered, kEntered);
  api->add(api->owner, reporter.callback_completed, kCompleted);
  api->add(api->owner, reporter.callback_threw_std, kThrewStd);
  api->add(api->owner, reporter.callback_threw_nonstd, kThrewNonstd);
  api->add(api->owner, reporter.released, 3);
  api->add_signed(api->owner, reporter.outstanding, 2);
  api->add_signed(api->owner, reporter.outstanding, -1);
  api->add(api->owner, reporter.outstanding_peak, 2);
  api->add(api->owner, reporter.limit_drop, 1);

  const auto snapshot = controller.registry().ReporterSnapshotJsonl();
  const auto expect_metric = [&](std::string_view metric, uint64_t value) {
    const auto record = std::string("\"metric\":\"") +
                        std::string(metric) +
                        "\",\"topic\":\"retained/topic\",\"type\":"
                        "\"RetainedType\",\"driver\":\"iceoryx\",\"value\":" +
                        std::to_string(value);
    EXPECT_NE(snapshot.find(record), std::string::npos) << metric;
  };

  size_t retained_records = 0;
  for (size_t pos = 0;
       (pos = snapshot.find("\"kind\":\"retained_iceoryx_receive\"", pos)) !=
       std::string::npos;
       ++retained_records, ++pos) {
  }
  EXPECT_EQ(retained_records, 9u);
  expect_metric("receive_delivered", 4);
  expect_metric("callback_entered", kEntered);
  expect_metric("callback_completed", kCompleted);
  expect_metric("callback_threw_std", kThrewStd);
  expect_metric("callback_threw_nonstd", kThrewNonstd);
  expect_metric("receive_released", 3);
  expect_metric("outstanding_current", 1);
  expect_metric("outstanding_peak", 2);
  expect_metric("limit_drop", 1);
  EXPECT_EQ(kEntered, kCompleted + kThrewStd + kThrewNonstd);
}
#endif

#if defined(IBMW_ENABLE_CORE_METRICS) && defined(IBMW_ENABLE_REPORTER_V1)
TEST_F(ReporterHostBoundary, BatchVisibilityRollbackAndIndependence) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* v1 = ibmw_reporter_query_v1();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(v1, nullptr);
  ASSERT_NE(batch, nullptr);
  ASSERT_EQ(batch->owner, v1->owner);

  static constexpr std::array<const char*, 9> metrics{
      "m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8"};
  const auto make_descriptors = [&](size_t count, const char* kind) {
    std::vector<ibmw_reporter_slot_descriptor_v1> descriptors(count);
    for (size_t i = 0; i < count; ++i) {
      descriptors[i].abi_version = IBMW_REPORTER_ABI_VERSION_V1;
      descriptors[i].struct_size = sizeof(descriptors[i]);
      descriptors[i].kind = {kind, std::strlen(kind)};
      descriptors[i].metric = {metrics[i], 2};
    }
    return descriptors;
  };
  const auto count_kind = [&](std::string_view kind) {
    const auto snapshot = controller.registry().ReporterSnapshotJsonl();
    const std::string needle = "\"kind\":\"" + std::string(kind) + "\"";
    size_t count = 0;
    for (size_t pos = 0; (pos = snapshot.find(needle, pos)) != std::string::npos;
         ++count, ++pos) {}
    return count;
  };

  auto six = make_descriptors(6, "batch_six");
  std::array<ibmw_reporter_slot_v1*, 6> six_slots{};
  ibmw_reporter_batch_transaction_v1* transaction = nullptr;
  ASSERT_TRUE(batch->prepare(batch->owner, six.data(), six.size(),
                             six_slots.data(), &transaction));
  EXPECT_EQ(count_kind("batch_six"), 0u);
  batch->abort(batch->owner, transaction);
  EXPECT_EQ(count_kind("batch_six"), 0u);

  for (size_t fail_at = 0; fail_at < six.size(); ++fail_at) {
    auto invalid = six;
    invalid[fail_at].metric = {nullptr, 0};
    transaction = nullptr;
    EXPECT_FALSE(batch->prepare(batch->owner, invalid.data(), invalid.size(),
                                six_slots.data(), &transaction));
    EXPECT_EQ(transaction, nullptr);
    EXPECT_EQ(count_kind("batch_six"), 0u);
  }

  ASSERT_TRUE(batch->prepare(batch->owner, six.data(), six.size(),
                             six_slots.data(), &transaction));
  ASSERT_TRUE(batch->commit(batch->owner, transaction));
  EXPECT_EQ(count_kind("batch_six"), 6u);

  auto nine = make_descriptors(9, "batch_nine");
  std::array<ibmw_reporter_slot_v1*, 9> nine_slots{};
  ASSERT_TRUE(batch->prepare(batch->owner, nine.data(), nine.size(),
                             nine_slots.data(), &transaction));
  EXPECT_EQ(count_kind("batch_nine"), 0u);
  ASSERT_TRUE(batch->commit(batch->owner, transaction));
  EXPECT_EQ(count_kind("batch_nine"), 9u);

  auto first = six_slots;
  ASSERT_TRUE(batch->prepare(batch->owner, six.data(), six.size(),
                             six_slots.data(), &transaction));
  ASSERT_TRUE(batch->commit(batch->owner, transaction));
  EXPECT_NE(first[0], six_slots[0]);
  EXPECT_EQ(count_kind("batch_six"), 12u);
}

TEST_F(ReporterHostBoundary, BatchOwnerMismatchAndOrdinaryV1FailClosed) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* v1 = ibmw_reporter_query_v1();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(v1, nullptr);
  ASSERT_NE(batch, nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"ordinary", 8};
  descriptor.metric = {"count", 5};
  EXPECT_NE(v1->register_slot(v1->owner, &descriptor),
            v1->register_slot(v1->owner, &descriptor));

  auto invalid = *batch;
  invalid.owner = &invalid;
  ibmw_reporter_slot_v1* slot = nullptr;
  ibmw_reporter_batch_transaction_v1* transaction = nullptr;
  EXPECT_FALSE(invalid.prepare(invalid.owner, &descriptor, 1, &slot,
                               &transaction));
  EXPECT_EQ(transaction, nullptr);
}

TEST_F(ReporterHostBoundary, BatchCommitAfterFreezeIsRejectedAndAbortIsSafe) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(batch, nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"lifecycle", 9};
  descriptor.metric = {"count", 5};
  ibmw_reporter_slot_v1* slot = nullptr;
  ibmw_reporter_batch_transaction_v1* transaction = nullptr;
  ASSERT_TRUE(batch->prepare(batch->owner, &descriptor, 1, &slot, &transaction));
  ASSERT_NE(transaction, nullptr);
  controller.registry().Freeze();
  EXPECT_FALSE(batch->commit(batch->owner, transaction));
  batch->abort(batch->owner, transaction);
  EXPECT_EQ(controller.registry().ReporterSnapshotJsonl().find("lifecycle"),
            std::string::npos);
}

TEST_F(ReporterHostBoundary, InvalidTokenAndCommitAfterStartFailClosed) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(batch, nullptr);
  auto* invalid = reinterpret_cast<ibmw_reporter_batch_transaction_v1*>(
      static_cast<uintptr_t>(0x1234));
  EXPECT_FALSE(batch->commit(batch->owner, invalid));
  batch->abort(batch->owner, invalid);
  controller.Start();
  EXPECT_FALSE(batch->commit(batch->owner, invalid));
}

TEST_F(ReporterHostBoundary, ResetRetiresStagedSlotsAndInvalidatesToken) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* v1 = ibmw_reporter_query_v1();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(v1, nullptr);
  ASSERT_NE(batch, nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"reset_staged", 12};
  descriptor.metric = {"count", 5};
  ibmw_reporter_slot_v1* slot = nullptr;
  ibmw_reporter_batch_transaction_v1* transaction = nullptr;
  ASSERT_TRUE(batch->prepare(batch->owner, &descriptor, 1, &slot, &transaction));
  controller.ResetForTesting();
  v1->add(v1->owner, slot, 1);
  EXPECT_FALSE(batch->commit(batch->owner, transaction));
  batch->abort(batch->owner, transaction);
}

TEST_F(ReporterHostBoundary, ConcurrentSnapshotSeesZeroOrWholeBatch) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  const auto* batch = ibmw_reporter_query_batch_v1();
  ASSERT_NE(batch, nullptr);
  constexpr size_t kCount = 9;
  std::array<ibmw_reporter_slot_descriptor_v1, kCount> descriptors{};
  std::array<ibmw_reporter_slot_v1*, kCount> slots{};
  static constexpr std::array<const char*, kCount> metrics{
      "m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8"};
  for (size_t i = 0; i < kCount; ++i) {
    descriptors[i].abi_version = IBMW_REPORTER_ABI_VERSION_V1;
    descriptors[i].struct_size = sizeof(descriptors[i]);
    descriptors[i].kind = {"atomic_batch", 12};
    descriptors[i].metric = {metrics[i], 2};
  }
  ibmw_reporter_batch_transaction_v1* transaction = nullptr;
  ASSERT_TRUE(batch->prepare(batch->owner, descriptors.data(), kCount,
                             slots.data(), &transaction));
  std::atomic_bool committed{false};
  std::thread commit([&] {
    EXPECT_TRUE(batch->commit(batch->owner, transaction));
    committed.store(true, std::memory_order_release);
  });
  do {
    const auto snapshot = controller.registry().ReporterSnapshotJsonl();
    size_t count = 0;
    for (size_t pos = 0;
         (pos = snapshot.find("\"kind\":\"atomic_batch\"", pos)) !=
         std::string::npos;
         ++count, ++pos) {
    }
    EXPECT_TRUE(count == 0 || count == kCount) << count;
  } while (!committed.load(std::memory_order_acquire));
  commit.join();
}
#endif
}  // namespace
