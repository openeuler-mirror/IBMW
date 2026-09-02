// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#ifdef IBMW_ENABLE_COMM_TRACE

#include <atomic>
#include <string>

#include "engine/transport/internal/comm_trace_send_overlay.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "observability/comm_trace.h"

namespace ibmw::runtime::core::transport {
namespace {

std::atomic<uint64_t> overlay_clock_reads{0};

uint64_t OverlayClock() noexcept {
  return 4242 + overlay_clock_reads.fetch_add(1, std::memory_order_relaxed);
}

class OverlayDriver final : public TransportDriver,
                            public internal::CommTraceSendOverlayV1 {
 public:
  explicit OverlayDriver(std::string name, bool valid_descriptor = true)
      : name_(std::move(name)),
        descriptor_{internal::kPublishEnvelopeAbiV1,
                    sizeof(internal::OverlayDescriptorV1),
                    sizeof(internal::PublishEnvelopeV1),
                    0,
                    valid_descriptor ? internal::kCommTraceSendOverlayIdV1 : 0} {}

  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("comm-trace-overlay-test")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper&) noexcept override { return true; }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override { return false; }
  void Send(MessageFrame&) noexcept override { ++normal_sends; }

  const internal::OverlayDescriptorV1& Descriptor() const noexcept override {
    return descriptor_;
  }

  void SendObserved(MessageFrame&,
                    const internal::PublishEnvelopeV1& envelope) noexcept override {
    ++observed_sends;
    observed_timestamp = envelope.publish_enter_ns;
  }

  int normal_sends = 0;
  int observed_sends = 0;
  uint64_t observed_timestamp = 0;

 private:
  std::string name_;
  internal::OverlayDescriptorV1 descriptor_;
};

SendAccessInfoWrapper SendRequest() {
  static const int message = 7;
  return SendAccessInfoWrapper{.pkg_path = "pkg",
                               .module_name = "module",
                               .topic_name = "topic",
                               .msg_type = ibmw_string_view_t{"Frame", 5},
                               .ctx_ptr = nullptr,
                               .msg_ptr = &message};
}

void RegisterFrame(TransportDriverDispatcher& dispatcher) {
  SendTypeWrapper wrapper;
  wrapper.info.msg_type = "Frame";
  wrapper.info.topic_name = "topic";
  wrapper.info.pkg_path = "pkg";
  wrapper.info.module_name = "module";
  ASSERT_TRUE(dispatcher.RegisterSendType(std::move(wrapper)));
}

class CommTraceDispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    observability::comm_trace::reset_for_test();
    overlay_clock_reads.store(0, std::memory_order_relaxed);
    observability::comm_trace::SetClockForTest(&OverlayClock);
  }
};

TEST_F(CommTraceDispatcherTest, FullModeUsesObservedSingleDriverPath) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  OverlayDriver driver("overlay");
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"overlay"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();
  RegisterFrame(dispatcher);
  dispatcher.Start();
  observability::comm_trace::SetClockForTest(&OverlayClock);
  observability::comm_trace::SetModeForTest(
      observability::comm_trace::RuntimeMode::full);

  dispatcher.Send(SendRequest());

  EXPECT_EQ(driver.observed_sends, 1);
  EXPECT_EQ(driver.normal_sends, 0);
  EXPECT_EQ(driver.observed_timestamp, 4242U);
}

TEST_F(CommTraceDispatcherTest, OffModeUsesNormalPathWithoutClockRead) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  OverlayDriver driver("overlay");
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"overlay"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();
  RegisterFrame(dispatcher);
  dispatcher.Start();
  observability::comm_trace::SetClockForTest(&OverlayClock);
  observability::comm_trace::SetModeForTest(
      observability::comm_trace::RuntimeMode::off);

  dispatcher.Send(SendRequest());

  EXPECT_EQ(driver.normal_sends, 1);
  EXPECT_EQ(driver.observed_sends, 0);
  EXPECT_EQ(overlay_clock_reads.load(std::memory_order_relaxed), 0U);
}

TEST_F(CommTraceDispatcherTest, MultipleDriversDoNotUseObservedPath) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  OverlayDriver first("first");
  OverlayDriver second("second");
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"first", "second"}}});
  dispatcher.RegisterTransportDriver(&first);
  dispatcher.RegisterTransportDriver(&second);
  dispatcher.Initialize();
  RegisterFrame(dispatcher);
  dispatcher.Start();
  observability::comm_trace::SetClockForTest(&OverlayClock);
  observability::comm_trace::SetModeForTest(
      observability::comm_trace::RuntimeMode::full);

  dispatcher.Send(SendRequest());

  EXPECT_EQ(first.normal_sends, 1);
  EXPECT_EQ(second.normal_sends, 1);
  EXPECT_EQ(first.observed_sends, 0);
  EXPECT_EQ(second.observed_sends, 0);
}

TEST_F(CommTraceDispatcherTest, InvalidOverlayDescriptorUsesNormalPath) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  OverlayDriver driver("overlay", false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"overlay"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();
  RegisterFrame(dispatcher);
  dispatcher.Start();
  observability::comm_trace::SetModeForTest(
      observability::comm_trace::RuntimeMode::full);

  dispatcher.Send(SendRequest());

  EXPECT_EQ(driver.normal_sends, 1);
  EXPECT_EQ(driver.observed_sends, 0);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport

#endif
