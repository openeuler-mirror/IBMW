// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <array>
#include <unordered_set>

#include "cpp/transport/transport_handle.h"
#include "engine/transport/transport_access.h"

namespace ibmw::runtime::core::transport {
namespace {

class FakePodDriver : public TransportDriver,
                      public LoanedPodRegistrationCapabilityV1 {
 public:
  std::string_view Name() const noexcept override { return "fake-pod"; }
  IBMW_DECLARE_DRIVER_SLOT("fake-pod")

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

  bool RegisterLoanedPodTopic(const TransportInfo& info, size_t size_hint,
                              size_t alignment) noexcept override {
    ++registration_calls;
    last_info = &info;
    last_size_hint = size_hint;
    last_alignment = alignment;
    return supports_registration;
  }

  bool LoanSample(const TransportInfo& info, size_t requested_size,
                  LoanMode mode, const ibmw::transport::Context*,
                  LoanedSampleHandle& out) noexcept override {
    ++loan_calls;
    EXPECT_EQ(&info, last_info);
    if (mode != LoanMode::kPodOnly || requested_size > storage.size()) {
      return false;
    }
    out.user_payload_ptr = storage.data();
    out.user_payload_size = requested_size;
    out.mode = mode;
    out.driver_token = storage.data();
    out.source_driver = this;
    return true;
  }

  void PublishLoaned(LoanedSampleHandle& handle) noexcept override {
    ++publish_calls;
    handle = {};
  }

  void ReleaseLoaned(LoanedSampleHandle& handle) noexcept override {
    ++release_calls;
    handle = {};
  }

  bool supports_registration = true;
  int registration_calls = 0;
  int loan_calls = 0;
  int publish_calls = 0;
  int release_calls = 0;
  size_t last_size_hint = 0;
  size_t last_alignment = 0;
  const TransportInfo* last_info = nullptr;
  std::array<std::byte, 256> storage{};
};

class PodRegistrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dispatcher.SetTransportCatalog(&catalog);
    dispatcher.SetPubTopicsDriversRules({{".*", {"fake-pod"}}});
    dispatcher.RegisterTransportDriver(
        &driver,
        TransportDriverCapabilitiesV1{
            .loaned_pod_registration_v1 =
                static_cast<LoanedPodRegistrationCapabilityV1*>(&driver)});
    dispatcher.Initialize();
  }

  ibmw::transport::SenderRef Sender(std::string_view topic = "camera") {
    sender_access = std::make_unique<SenderAccess>(
        "pkg", "module", topic, logger, dispatcher, passed_keys);
    return ibmw::transport::SenderRef(sender_access->NativeHandle());
  }

  ibmw::common::util::LoggerWrapper logger;
  std::unordered_set<std::string> passed_keys;
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  FakePodDriver driver;
  std::unique_ptr<SenderAccess> sender_access;
};

TEST_F(PodRegistrationTest, PublicRegisterThenStartLoanPublishAndRelease) {
  auto sender = Sender();
  ASSERT_TRUE(sender.RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_TRUE(sender.RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_EQ(driver.registration_calls, 1);
  EXPECT_EQ(driver.last_size_hint, 128u);
  EXPECT_EQ(driver.last_alignment, 64u);

  dispatcher.Start();
  auto publish = sender.Loan("Frame", 96);
  ASSERT_NE(publish.user_payload_ptr, nullptr);
  sender.PublishLoaned(publish);
  EXPECT_EQ(driver.publish_calls, 1);
  EXPECT_EQ(publish.source_driver, nullptr);

  auto release = sender.Loan("Frame", 32);
  ASSERT_NE(release.user_payload_ptr, nullptr);
  sender.ReleaseLoaned(release);
  EXPECT_EQ(driver.release_calls, 1);
  EXPECT_EQ(release.source_driver, nullptr);
}

TEST_F(PodRegistrationTest, RejectsInvalidConflictingAndLateRegistration) {
  auto sender = Sender();
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 0, 64));
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 128, 0));
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 128, 3));
  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    EXPECT_FALSE(sender.RegisterLoanedPodTopic(
        "Frame", static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1,
        64));
  }
  EXPECT_TRUE(sender.RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 256, 64));
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 128, 32));
  EXPECT_EQ(driver.registration_calls, 1);

  dispatcher.Start();
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Other", 64, 64));
}

class RoutedPodDriver final : public FakePodDriver {
 public:
  RoutedPodDriver(std::string name, bool supports_pod)
      : name_(std::move(name)) {
    supports_registration = supports_pod;
  }
  std::string_view Name() const noexcept override { return name_; }

 private:
  std::string name_;
};

TEST(PodRegistrationRouting, SelectedPodDriverWinsOverNormalCatalog) {
  ibmw::common::util::LoggerWrapper logger;
  std::unordered_set<std::string> passed_keys;
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  RoutedPodDriver first("normal-first", false);
  RoutedPodDriver selected("pod-selected", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPubTopicsDriversRules(
      {{".*", {"normal-first", "pod-selected"}}});
  dispatcher.RegisterTransportDriver(&first);
  dispatcher.RegisterTransportDriver(
      &selected,
      TransportDriverCapabilitiesV1{
          .loaned_pod_registration_v1 =
              static_cast<LoanedPodRegistrationCapabilityV1*>(&selected)});
  dispatcher.Initialize();

  auto normal = std::make_unique<SendTypeWrapper>();
  normal->info.msg_type = "Frame";
  normal->info.topic_name = "camera";
  normal->info.pkg_path = "pkg";
  normal->info.module_name = "module";
  normal->cached_drivers = {&first, &selected};
  ASSERT_TRUE(catalog.RegisterSendType(std::move(normal)));

  SenderAccess access("pkg", "module", "camera", logger, dispatcher,
                      passed_keys);
  ibmw::transport::SenderRef sender(access.NativeHandle());
  ASSERT_TRUE(sender.RegisterLoanedPodTopic("Frame", 128, 64));
  dispatcher.Start();

  EXPECT_NE(sender.Loan("Frame", 32).user_payload_ptr, nullptr);
  EXPECT_EQ(first.loan_calls, 0);
  EXPECT_EQ(selected.loan_calls, 1);
}

TEST_F(PodRegistrationTest, FailedDriverRegistrationDoesNotEnableLoan) {
  driver.supports_registration = false;
  auto sender = Sender();
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_EQ(driver.registration_calls, 1);

  dispatcher.Start();
  EXPECT_EQ(sender.Loan("Frame", 32).user_payload_ptr, nullptr);
  EXPECT_EQ(driver.loan_calls, 0);
}

TEST(PodRegistrationRouting, LegacyDriverWithoutCapabilityFailsClearly) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  RoutedPodDriver legacy("legacy", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPubTopicsDriversRules({{".*", {"legacy"}}});
  dispatcher.RegisterTransportDriver(&legacy);
  dispatcher.Initialize();

  ibmw::common::util::LoggerWrapper logger;
  std::unordered_set<std::string> passed_keys;
  SenderAccess access("pkg", "module", "camera", logger, dispatcher,
                      passed_keys);
  EXPECT_FALSE(ibmw::transport::SenderRef(access.NativeHandle())
                   .RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_EQ(legacy.registration_calls, 0);
}

TEST(PodRegistrationRouting, CapabilityFailureDoesNotFallback) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  RoutedPodDriver rejecting("rejecting", false);
  RoutedPodDriver fallback("fallback", true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPubTopicsDriversRules(
      {{".*", {"rejecting", "fallback"}}});
  dispatcher.RegisterTransportDriver(
      &rejecting,
      TransportDriverCapabilitiesV1{
          .loaned_pod_registration_v1 =
              static_cast<LoanedPodRegistrationCapabilityV1*>(&rejecting)});
  dispatcher.RegisterTransportDriver(
      &fallback,
      TransportDriverCapabilitiesV1{
          .loaned_pod_registration_v1 =
              static_cast<LoanedPodRegistrationCapabilityV1*>(&fallback)});
  dispatcher.Initialize();

  ibmw::common::util::LoggerWrapper logger;
  std::unordered_set<std::string> passed_keys;
  SenderAccess access("pkg", "module", "camera", logger, dispatcher,
                      passed_keys);
  EXPECT_FALSE(ibmw::transport::SenderRef(access.NativeHandle())
                   .RegisterLoanedPodTopic("Frame", 128, 64));
  EXPECT_EQ(rejecting.registration_calls, 1);
  EXPECT_EQ(fallback.registration_calls, 0);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
