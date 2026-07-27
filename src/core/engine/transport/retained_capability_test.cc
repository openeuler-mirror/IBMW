// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "cpp/transport/transport_handle.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_driver_dispatcher.h"

namespace ibmw::runtime::core::transport {
namespace {

class FakeSubscription final : public LoanedSubscriptionHandle {
 public:
  void Stop() noexcept override {}
  bool Close(std::chrono::milliseconds) noexcept override { return true; }
};

class FakeRetainedDriver : public TransportDriver,
                           public RetainedLoanCapabilityV1 {
 public:
  FakeRetainedDriver(std::string name, bool succeeds)
      : name_(std::move(name)), succeeds_(succeeds) {}

  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("fake-retained")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override {
    return false;
  }
  void Send(MessageFrame&) noexcept override {}

  std::unique_ptr<LoanedSubscriptionHandle> AttachLoanedRetained(
      const LoanedMessageHandlerWrapper&, RetainedLoanOptions) noexcept override {
    ++calls;
    if (!succeeds_) return nullptr;
    return std::make_unique<FakeSubscription>();
  }

  int calls = 0;

 private:
  std::string name_;
  bool succeeds_;
};

ibmw_type_support_base_t MakeTypeSupport() {
  ibmw_type_support_base_t result{};
  result.type_name = [](void*) {
    return ibmw::util::ToIbmwStringView("Frame");
  };
  return result;
}

std::unique_ptr<LoanedSubscriptionHandle> Attach(
    TransportDriverDispatcher& dispatcher) {
  auto type_support = MakeTypeSupport();
  ibmw::transport::LoanedMessageCallback callback(
      [](const ibmw_transport_context_base_t*, const void*, size_t,
         ibmw_function_base_t*) {});
  return dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = "camera",
          .msg_type_support = &type_support,
          .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
}

TEST(RetainedCapabilityRouting, SkipsLegacyDriverAndUsesNextCapability) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FakeRetainedDriver legacy("legacy", true);
  FakeRetainedDriver capable("capable", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"legacy", "capable"}}});
  dispatcher.RegisterTransportDriver(&legacy);
  dispatcher.RegisterTransportDriver(
      &capable,
      TransportDriverCapabilitiesV1{
          .retained_loan_v1 =
              static_cast<RetainedLoanCapabilityV1*>(&capable)});
  dispatcher.Initialize();

  EXPECT_NE(Attach(dispatcher), nullptr);
  EXPECT_EQ(legacy.calls, 0);
  EXPECT_EQ(capable.calls, 1);
}

TEST(RetainedCapabilityRouting, CapabilityFailureFallsBackToNextDriver) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FakeRetainedDriver rejecting("rejecting", false);
  FakeRetainedDriver fallback("fallback", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules(
      {{".*", {"rejecting", "fallback"}}});
  dispatcher.RegisterTransportDriver(
      &rejecting,
      TransportDriverCapabilitiesV1{
          .retained_loan_v1 =
              static_cast<RetainedLoanCapabilityV1*>(&rejecting)});
  dispatcher.RegisterTransportDriver(
      &fallback,
      TransportDriverCapabilitiesV1{
          .retained_loan_v1 =
              static_cast<RetainedLoanCapabilityV1*>(&fallback)});
  dispatcher.Initialize();

  EXPECT_NE(Attach(dispatcher), nullptr);
  EXPECT_EQ(rejecting.calls, 1);
  EXPECT_EQ(fallback.calls, 1);
}

TEST(RetainedCapabilityRouting, LegacyDriverWithoutCapabilityFailsClearly) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FakeRetainedDriver legacy("legacy", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"legacy"}}});
  dispatcher.RegisterTransportDriver(&legacy);
  dispatcher.Initialize();

  EXPECT_EQ(Attach(dispatcher), nullptr);
  EXPECT_EQ(legacy.calls, 0);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
