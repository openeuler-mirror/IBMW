// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// COMM-025: Stop() waits for callbacks running on other threads, but not for
// the one it is called from.

#include "engine/topic_io/cached_transport_subscription_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "cpp/util/string.h"

namespace ibmw::runtime::core::topic_io {
namespace {

using namespace std::chrono_literals;
namespace transport = ibmw::runtime::core::transport;

class FakeDriver final : public transport::TransportDriver {
 public:
  std::string_view Name() const noexcept override { return "fake"; }
  IBMW_DECLARE_DRIVER_SLOT("fake-cached-subscription")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const transport::SendTypeWrapper&) noexcept override { return true; }
  bool OnMessage(const transport::MessageHandlerWrapper& wrapper) noexcept override {
    handler = &wrapper;
    return true;
  }
  void Send(transport::MessageFrame&) noexcept override {}

  void Deliver() {
    const std::string payload = "payload";
    transport::MessageFrame frame{.info = handler->info};
    frame.serialization_cache.emplace(
        "raw", std::make_shared<ibmw::util::BufferArrayView>(payload.data(), payload.size()));
    handler->callback(frame, {}, nullptr);
  }

  const transport::MessageHandlerWrapper* handler = nullptr;
};

ibmw_type_support_base_t MakeTypeSupport() {
  ibmw_type_support_base_t base{};
  base.type_name = [](void*) { return ibmw::util::ToIbmwStringView("Frame"); };
  return base;
}

class CachedSubscriptionStopTest : public testing::Test {
 protected:
  CachedSubscriptionStopTest() : registry_(logger_) {
    advertisement_.runtime_id = "runtime";
    advertisement_.publisher_id = "publisher";
    advertisement_.type_support_package = "pkg";
    advertisement_.topic.topic = "camera";
    advertisement_.topic.msg_type = "Frame";
    advertisement_.topic.serialization_type = "raw";
    source_.emplace(
        "fake", "module", driver_, advertisement_, ibmw::util::TypeSupportRef(&type_support_),
        registry_, ibmw::transport::QosDescriptor{});
  }

  ibmw::common::util::LoggerWrapper logger_;
  CachedTransportSubscriptionRegistry registry_;
  FakeDriver driver_;
  ibmw_type_support_base_t type_support_ = MakeTypeSupport();
  TopicAdvertisement advertisement_;
  std::optional<CachedTransportSubscriptionSource> source_;
};

TEST_F(CachedSubscriptionStopTest, StopFromCallbackDoesNotDeadlock) {
  int deliveries = 0;
  source_->Start([&](ibmw::transport::SerializedMessage&&) {
    ++deliveries;
    source_->Stop();
  });
  driver_.Deliver();
  EXPECT_EQ(deliveries, 1);

  driver_.Deliver();  // stopped: not delivered
  EXPECT_EQ(deliveries, 1);
}

TEST_F(CachedSubscriptionStopTest, StopWaitsForCallbackOnOtherThread) {
  std::promise<void> entered;
  std::promise<void> leave;
  source_->Start([&](ibmw::transport::SerializedMessage&&) {
    entered.set_value();
    leave.get_future().wait();
  });
  std::thread delivery([this] { driver_.Deliver(); });
  entered.get_future().wait();

  auto stopped = std::async(std::launch::async, [this] { source_->Stop(); });
  EXPECT_EQ(stopped.wait_for(100ms), std::future_status::timeout);

  leave.set_value();
  EXPECT_EQ(stopped.wait_for(10s), std::future_status::ready);
  delivery.join();
}

}  // namespace
}  // namespace ibmw::runtime::core::topic_io
