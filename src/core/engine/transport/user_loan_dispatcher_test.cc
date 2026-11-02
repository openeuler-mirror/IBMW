// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/transport/transport_driver_dispatcher.h"
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace ibmw::runtime::core::transport {
namespace {

class FakeUserLoanDriver final : public TransportDriver {
 public:
  enum class Result { kFailDirty, kWrongSource, kMinimalSuccess, kSuccess };

  FakeUserLoanDriver(std::string name, Result result)
      : name_(std::move(name)), result_(result) {}

  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("fake-user-loan")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper&) noexcept override { return true; }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override { return false; }
  void Send(MessageFrame&) noexcept override {}

  bool LoanSample(const TransportInfo&, size_t requested_size, LoanMode mode,
                  const ibmw::transport::Context*,
                  LoanedSampleHandle& out) noexcept override {
    ++loan_calls;
    saw_clean_input &= out.user_payload_ptr == nullptr &&
                       out.user_payload_size == 0 && out.driver_token == nullptr &&
                       out.source_driver == nullptr;
    out.user_payload_ptr = storage.data();
    if (result_ == Result::kMinimalSuccess) {
      out.source_driver = this;
      return true;
    }
    out.user_payload_size = requested_size;
    out.mode = mode;
    out.driver_token = storage.data();
    if (result_ == Result::kFailDirty) {
      out.source_driver = this;
      return false;
    }
    out.source_driver = result_ == Result::kWrongSource ? wrong_source : this;
    return true;
  }

  void PublishLoaned(LoanedSampleHandle&) noexcept override { ++publish_calls; }
  void ReleaseLoaned(LoanedSampleHandle&) noexcept override { ++release_calls; }

  TransportDriver* wrong_source = nullptr;
  int loan_calls = 0;
  int publish_calls = 0;
  int release_calls = 0;
  bool saw_clean_input = true;

 private:
  std::string name_;
  Result result_;
  std::array<std::byte, 64> storage{};
};

LoanAccessInfoWrapper LoanRequest() {
  return LoanAccessInfoWrapper{.pkg_path = "pkg",
                               .module_name = "module",
                               .topic_name = "camera",
                               .msg_type = ibmw_string_view_t{"Frame", 5},
                               .requested_size = 32,
                               .mode = LoanMode::kCtxMetaThenPayload};
}

TEST(UserLoanDispatcher, IsolatesFailedAttemptsAndConsumesOnce) {
#ifdef IBMW_ENABLE_CORE_METRICS
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& metrics = observability::CoreMetricsController();
#endif
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FrameworkAsyncTransportFilterBus publish_filters;
  FakeUserLoanDriver dirty("dirty", FakeUserLoanDriver::Result::kFailDirty);
  FakeUserLoanDriver good("good", FakeUserLoanDriver::Result::kSuccess);

#ifdef IBMW_ENABLE_CORE_METRICS
  metrics.Initialize(&dispatcher);
#endif
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"dirty", "good"}}});
  dispatcher.RegisterTransportDriver(&dirty);
  dispatcher.RegisterTransportDriver(&good);
  dispatcher.Initialize();

  SendTypeWrapper wrapper;
  wrapper.info.msg_type = "Frame";
  wrapper.info.topic_name = "camera";
  wrapper.info.pkg_path = "pkg";
  wrapper.info.module_name = "module";
  ASSERT_TRUE(dispatcher.RegisterSendType(std::move(wrapper)));
#ifdef IBMW_ENABLE_CORE_METRICS
  metrics.Start(&dispatcher);
#endif
  dispatcher.Start();

  int tap_calls = 0;
  const auto tap_handle =
      dispatcher.RegisterPublishTap([&](MessageFrame&) { ++tap_calls; });
  ASSERT_NE(tap_handle, 0U);

  auto publish = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(publish.source_driver, &good);
  EXPECT_TRUE(dirty.saw_clean_input);
  EXPECT_TRUE(good.saw_clean_input);
  EXPECT_EQ(dirty.loan_calls, 1);
  EXPECT_EQ(good.loan_calls, 1);
  dispatcher.PublishLoaned(publish);
  dispatcher.ReleaseLoaned(publish);
  EXPECT_EQ(good.publish_calls, 1);
  EXPECT_EQ(good.release_calls, 0);
  EXPECT_EQ(tap_calls, 0)
      << "The documented zero-copy fast path must bypass publish taps";

  auto release = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(release.source_driver, &good);
  dispatcher.ReleaseLoaned(release);
  dispatcher.PublishLoaned(release);
  EXPECT_EQ(good.publish_calls, 1);
  EXPECT_EQ(good.release_calls, 1);
  EXPECT_TRUE(dispatcher.UnregisterPublishTap(tap_handle));

  // The established handle contract routes solely by source_driver. Side data
  // and dispatcher membership are driver-private and must not be validated by
  // observability or dispatcher glue.
  FakeUserLoanDriver outsider("outsider", FakeUserLoanDriver::Result::kSuccess);
  LoanedSampleHandle forged;
  forged.source_driver = &outsider;
  dispatcher.PublishLoaned(forged);
  EXPECT_EQ(forged.source_driver, nullptr);
  EXPECT_EQ(outsider.publish_calls, 1);

  forged.source_driver = &outsider;
  dispatcher.ReleaseLoaned(forged);
  EXPECT_EQ(forged.source_driver, nullptr);
  EXPECT_EQ(outsider.release_calls, 1);

#ifdef IBMW_ENABLE_CORE_METRICS
  const auto json = metrics.registry().SnapshotJsonl();
  EXPECT_NE(json.find("\"loan_attempted\":2"), std::string::npos);
  EXPECT_NE(json.find("\"loan_succeeded\":2"), std::string::npos);
  EXPECT_NE(json.find("\"loan_publish_called\":1"), std::string::npos);
  EXPECT_NE(json.find("\"loan_release_called\":1"), std::string::npos);
  dispatcher.Shutdown();
  EXPECT_TRUE(metrics.FinalizeSnapshot(&dispatcher));
  unsetenv("IBMW_CORE_METRICS");
#endif
}

TEST(UserLoanDispatcher, ReleaseAfterShutdownDropsHandleWithoutDriverCall) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FrameworkAsyncTransportFilterBus publish_filters;
  FakeUserLoanDriver good("good", FakeUserLoanDriver::Result::kSuccess);

  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"good"}}});
  dispatcher.RegisterTransportDriver(&good);
  dispatcher.Initialize();

  SendTypeWrapper wrapper;
  wrapper.info.msg_type = "Frame";
  wrapper.info.topic_name = "camera";
  wrapper.info.pkg_path = "pkg";
  wrapper.info.module_name = "module";
  ASSERT_TRUE(dispatcher.RegisterSendType(std::move(wrapper)));
  dispatcher.Start();

  auto outstanding = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(outstanding.source_driver, &good);
  dispatcher.Shutdown();

  // After Shutdown the source driver may already be destroyed by its owner.
  dispatcher.ReleaseLoaned(outstanding);
  EXPECT_EQ(outstanding.source_driver, nullptr);
  EXPECT_EQ(outstanding.driver_token, nullptr);
  EXPECT_EQ(good.release_calls, 0);
}

TEST(UserLoanDispatcher, BooleanSuccessIsNotRejectedByMetricsSideInvariants) {
#ifdef IBMW_ENABLE_CORE_METRICS
  unsetenv("IBMW_CORE_METRICS");
  auto& metrics = observability::CoreMetricsController();
  metrics.ResetForTesting();
#endif
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FrameworkAsyncTransportFilterBus publish_filters;
  FakeUserLoanDriver driver("minimal",
                            FakeUserLoanDriver::Result::kMinimalSuccess);
#ifdef IBMW_ENABLE_CORE_METRICS
  metrics.Initialize(&dispatcher);
#endif
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"minimal"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();

  SendTypeWrapper wrapper;
  wrapper.info.msg_type = "Frame";
  wrapper.info.topic_name = "camera";
  wrapper.info.pkg_path = "pkg";
  wrapper.info.module_name = "module";
  ASSERT_TRUE(dispatcher.RegisterSendType(std::move(wrapper)));
  dispatcher.Start();

  auto handle = dispatcher.Loan(LoanRequest());
  EXPECT_EQ(handle.source_driver, &driver);
  EXPECT_NE(handle.user_payload_ptr, nullptr);
  EXPECT_EQ(handle.driver_token, nullptr);
  handle.user_payload_ptr = nullptr;
  dispatcher.PublishLoaned(handle);
  EXPECT_EQ(driver.publish_calls, 1);
  EXPECT_EQ(handle.source_driver, nullptr);

  handle = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(handle.source_driver, &driver);
  handle.user_payload_ptr = nullptr;
  dispatcher.ReleaseLoaned(handle);
  EXPECT_EQ(driver.release_calls, 1);
  EXPECT_EQ(handle.source_driver, nullptr);
#ifdef IBMW_ENABLE_CORE_METRICS
  EXPECT_TRUE(metrics.FinalizeSnapshot(&dispatcher));
#endif
}

#ifdef IBMW_ENABLE_CORE_METRICS
TEST(UserLoanMetricsIntegration, RealDispatcherWorkloadWritesFinalSnapshot) {
  const char* metrics_mode = std::getenv("IBMW_CORE_METRICS");
  const char* output_dir = std::getenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
  if (metrics_mode == nullptr || std::string_view(metrics_mode) != "counters" ||
      output_dir == nullptr || *output_dir == '\0') {
    GTEST_SKIP() << "Set IBMW_CORE_METRICS=counters and "
                    "IBMW_OBSERVABILITY_OUTPUT_DIR externally";
  }

  const auto metrics_path = std::filesystem::path(output_dir) / "metrics.jsonl";
  std::error_code remove_error;
  std::filesystem::remove(metrics_path, remove_error);

  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FrameworkAsyncTransportFilterBus publish_filters;
  FakeUserLoanDriver driver("metrics-user-loan",
                            FakeUserLoanDriver::Result::kSuccess);
  auto& metrics = observability::CoreMetricsController();

  metrics.Initialize(&dispatcher);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"metrics-user-loan"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();

  SendTypeWrapper wrapper;
  wrapper.info.msg_type = "Frame";
  wrapper.info.topic_name = "camera";
  wrapper.info.pkg_path = "pkg";
  wrapper.info.module_name = "module";
  ASSERT_TRUE(dispatcher.RegisterSendType(std::move(wrapper)));

  metrics.Start(&dispatcher);
  dispatcher.Start();

  auto publish = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(publish.source_driver, &driver);
  dispatcher.PublishLoaned(publish);

  auto release = dispatcher.Loan(LoanRequest());
  ASSERT_EQ(release.source_driver, &driver);
  dispatcher.ReleaseLoaned(release);

  EXPECT_EQ(driver.loan_calls, 2);
  EXPECT_EQ(driver.publish_calls, 1);
  EXPECT_EQ(driver.release_calls, 1);

  ASSERT_TRUE(metrics.FinalizeSnapshot(&dispatcher));
  std::ifstream input(metrics_path);
  ASSERT_TRUE(input.is_open()) << metrics_path;
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto jsonl = contents.str();
  EXPECT_NE(jsonl.find("\"kind\":\"logical_loan\""), std::string::npos);
  EXPECT_NE(jsonl.find("\"loan_attempted\":2"), std::string::npos);
  EXPECT_NE(jsonl.find("\"loan_succeeded\":2"), std::string::npos);
  EXPECT_NE(jsonl.find("\"kind\":\"source_driver_loan\""),
            std::string::npos);
  EXPECT_NE(jsonl.find("\"driver\":\"metrics-user-loan\""),
            std::string::npos);
  EXPECT_NE(jsonl.find("\"loan_publish_called\":1"), std::string::npos);
  EXPECT_NE(jsonl.find("\"loan_release_called\":1"), std::string::npos);
}
#endif

}  // namespace
}  // namespace ibmw::runtime::core::transport
