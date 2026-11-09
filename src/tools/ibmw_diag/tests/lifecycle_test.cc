// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <functional>
#include <stdexcept>
#include <string>

#include "engine/module/module_bus.h"
#include "engine/runtime_graph/module_lifecycle.h"
#include "gtest/gtest.h"

namespace ibmw::diag {
namespace {
namespace graph = runtime::core::runtime_graph;
using Result = extension::ModuleLifecycleResult;

// A C ABI module keeps deliberate exceptions visible at the ModuleBus boundary.
struct TestModule {
  std::string name = "camera";
  unsigned setup_calls = 0, run_calls = 0, stop_calls = 0;
  std::function<bool()> setup = [] { return true; };
  std::function<bool()> run = [] { return true; };
  ibmw_module_base_t base{
      .info = [](void* context) -> ibmw_module_info_t {
        return {.module_name = util::ToIbmwStringView(static_cast<TestModule*>(context)->name)};
      },
      .setup = [](void* context, const ibmw_core_base_t*) {
        auto& module = *static_cast<TestModule*>(context);
        ++module.setup_calls;
        return module.setup();
      },
      .run = [](void* context) {
        auto& module = *static_cast<TestModule*>(context);
        ++module.run_calls;
        return module.run();
      },
      .stop = [](void* context) { ++static_cast<TestModule*>(context)->stop_calls; },
      .ctx = this};
};

class DiagLifecycle : public testing::Test {
 protected:
  void SetUp() override {
    bus.RegisterCoreAccessConfigurator([](const auto&, auto&) {});
    bus.RegisterModule("unit-package", &module.base);
  }
  void TearDown() override { bus.Shutdown(); }
  TestModule module;
  runtime::core::module::ModuleBus bus;
};

TEST_F(DiagLifecycle, QueriesAreValueSnapshotsAndNeverInvokeCallbacks) {
  EXPECT_TRUE(bus.GetModuleLifecycleStatuses().empty());
  bus.Initialize(YAML::Node{});
  auto initialized = bus.GetModuleLifecycleStatuses();
  ASSERT_EQ(initialized.size(), 1u);
  EXPECT_EQ(initialized[0].initialize, Result::kSucceeded);
  EXPECT_EQ(initialized[0].start, Result::kNotAttempted);
  EXPECT_EQ(initialized[0].package_path, "unit-package");
  initialized[0].module_name = "changed-copy";
  EXPECT_EQ(bus.GetModuleLifecycleStatuses()[0].module_name, "camera");
  bus.Start();
  const auto started = bus.GetModuleLifecycleStatuses();
  EXPECT_EQ(started[0].start, Result::kSucceeded);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(bus.GetModuleLifecycleStatuses(), started);
  EXPECT_EQ(module.setup_calls, 1u);
  EXPECT_EQ(module.run_calls, 1u);
  bus.Shutdown();
  EXPECT_EQ(bus.GetModuleLifecycleStatuses(), started);
  EXPECT_EQ(module.stop_calls, 1u);
}

TEST_F(DiagLifecycle, InitializationFailureRetainsReasonAndDoesNotAttemptStart) {
  module.setup = [] { return false; };
  EXPECT_THROW(bus.Initialize(YAML::Node{}), std::exception);
  const auto snapshot = bus.GetModuleLifecycleStatuses();
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot[0].initialize, Result::kFailed);
  EXPECT_EQ(snapshot[0].start, Result::kNotAttempted);
  ASSERT_TRUE(snapshot[0].failure);
  EXPECT_EQ(snapshot[0].failure->phase, extension::ModuleLifecyclePhase::kInitialize);
  EXPECT_FALSE(snapshot[0].failure->reason.empty());
  EXPECT_EQ(module.run_calls, 0u);
}

TEST_F(DiagLifecycle, StartExceptionPreservesItsTypeAndEarlierSuccess) {
  module.run = []() -> bool { throw std::domain_error("hardware offline"); };
  bus.Initialize(YAML::Node{});
  EXPECT_THROW(bus.Start(), std::domain_error);
  const auto snapshot = bus.GetModuleLifecycleStatuses();
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot[0].initialize, Result::kSucceeded);
  EXPECT_EQ(snapshot[0].start, Result::kFailed);
  ASSERT_TRUE(snapshot[0].failure);
  EXPECT_EQ(snapshot[0].failure->reason, "hardware offline");
  EXPECT_EQ(snapshot[0].failure->phase, extension::ModuleLifecyclePhase::kStart);
}

TEST_F(DiagLifecycle, NonStandardExceptionIsRecordedAndRethrown) {
  module.setup = []() -> bool { throw 7; };
  EXPECT_THROW(bus.Initialize(YAML::Node{}), int);
  const auto snapshot = bus.GetModuleLifecycleStatuses();
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot[0].initialize, Result::kFailed);
  ASSERT_TRUE(snapshot[0].failure);
  EXPECT_NE(snapshot[0].failure->reason.find("Non-standard"), std::string::npos);
}

TEST_F(DiagLifecycle, CallbacksCanReadTheirInProgressState) {
  module.setup = [&] {
    EXPECT_EQ(bus.GetModuleLifecycleStatuses().front().initialize, Result::kInProgress);
    return true;
  };
  module.run = [&] {
    EXPECT_EQ(bus.GetModuleLifecycleStatuses().front().start, Result::kInProgress);
    return true;
  };
  bus.Initialize(YAML::Node{});
  bus.Start();
}

TEST_F(DiagLifecycle, DisabledModuleDoesNotCreateAFailureRecord) {
  bus.Initialize(YAML::Load("modules: [{name: camera, enable: false}]"));
  bus.Start();
  EXPECT_TRUE(bus.GetModuleLifecycleStatuses().empty());
  EXPECT_EQ(module.setup_calls, 0u);
  EXPECT_EQ(module.run_calls, 0u);
}

TEST_F(DiagLifecycle, AppContextHasNoManagedStartAndRecordsAreSorted) {
  bus.Initialize(YAML::Node{});
  ASSERT_NE(bus.CreateModule("app-package", "app"), nullptr);
  bus.Start();
  const auto snapshot = bus.GetModuleLifecycleStatuses();
  ASSERT_EQ(snapshot.size(), 2u);
  EXPECT_EQ(snapshot[0].module_name, "app");
  EXPECT_EQ(snapshot[0].kind, extension::ModuleLifecycleKind::kAppContext);
  EXPECT_EQ(snapshot[0].initialize, Result::kSucceeded);
  EXPECT_EQ(snapshot[0].start, Result::kNotApplicable);
  EXPECT_EQ(snapshot[1].module_name, "camera");
}

TEST(DiagLifecycleProtocol, RecordedFailureAndOptionalFieldsRoundTrip) {
  extension::ModuleLifecycleStatus item{
      .module_name = "相机", .package_path = "package.so",
      .initialize = Result::kSucceeded, .start = Result::kFailed,
      .failure = extension::ModuleLifecycleFailure{extension::ModuleLifecyclePhase::kStart, "failure\nreason"},
      .updated_at_unix_ms = 123456789};
  EXPECT_EQ(graph::ModuleLifecycleStatusFromJson(graph::ModuleLifecycleStatusToJson(item)), item);
  item.failure.reset();
  item.start = Result::kNotAttempted;
  EXPECT_EQ(graph::ModuleLifecycleStatusFromJson(graph::ModuleLifecycleStatusToJson(item)), item);
}

TEST(DiagLifecycleProtocol, UnknownEnumsMissingFieldsAndInvalidTimestampsFail) {
  extension::ModuleLifecycleStatus item{.module_name = "camera"};
  const auto valid = graph::ModuleLifecycleStatusToJson(item);
  for (const auto* field : {"module_name", "kind", "initialize", "start", "failure", "updated_at_unix_ms"}) {
    auto value = valid;
    value.erase(field);
    EXPECT_THROW(graph::ModuleLifecycleStatusFromJson(value), std::exception) << field;
  }
  for (const auto* field : {"kind", "initialize", "start"}) {
    auto value = valid;
    value[field] = "unknown-future-value";
    EXPECT_THROW(graph::ModuleLifecycleStatusFromJson(value), std::exception) << field;
  }
  for (const nlohmann::json& timestamp : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("123")}) {
    auto value = valid;
    value["updated_at_unix_ms"] = timestamp;
    EXPECT_THROW(graph::ModuleLifecycleStatusFromJson(value), std::exception);
  }
}
}  // namespace
}  // namespace ibmw::diag
