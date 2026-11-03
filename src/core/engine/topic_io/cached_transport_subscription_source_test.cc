// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// COMM-025: Stop() waits for callbacks running on other threads, but not for
// the one it is called from.
// COMM-024: loaned payloads smaller than the type, or of unknown size, are
// dropped and released without reaching the subscriber.

#include "engine/topic_io/cached_transport_subscription_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

  bool AttachLoaned(const transport::LoanedMessageHandlerWrapper& wrapper) noexcept override {
    loaned = &wrapper;
    return true;
  }

  // Delivers a loan of `size` bytes; returns how often it was released.
  int DeliverLoan(const void* payload, size_t size) {
    int released = 0;
    loaned->callback(nullptr, payload, size, [&released] { ++released; });
    return released;
  }

  const transport::MessageHandlerWrapper* handler = nullptr;
  const transport::LoanedMessageHandlerWrapper* loaned = nullptr;
};

const ibmw_string_view_t kRaw[] = {{"raw", 3}};

ibmw_type_support_base_t MakeTypeSupport() {
  ibmw_type_support_base_t base{};
  base.type_name = [](void*) { return ibmw::util::ToIbmwStringView("Frame"); };
  base.serialize = [](void*, ibmw_string_view_t, const void* msg,
                      const ibmw_buffer_array_allocator_t* allocator, ibmw_buffer_array_t* buffers) {
    auto buffer = allocator->allocate(allocator->ctx, buffers, sizeof(uint64_t));
    if (buffer.data == nullptr) return false;
    std::memcpy(buffer.data, msg, sizeof(uint64_t));
    return true;
  };
  base.serialization_types_supported_num = [](void*) -> size_t { return 1; };
  base.serialization_types_supported_list = [](void*) { return kRaw; };
  base.msg_min_size = sizeof(uint64_t);
  return base;
}

class CachedSubscriptionStopTest : public testing::Test {
 protected:
  CachedSubscriptionStopTest() : registry_(logger_) {
    logger_.get_log_level_func = [] { return ibmw::common::util::kLogLevelTrace; };
    logger_.log_func = [this](uint32_t level, uint32_t, const char*, const char*, const char* text, size_t) {
      if (level == ibmw::common::util::kLogLevelWarn && std::string_view(text).find("Dropping loaned") != std::string_view::npos) {
        ++drop_warnings_;
      }
    };
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
  int drop_warnings_ = 0;
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

TEST_F(CachedSubscriptionStopTest, UndersizedLoanIsDroppedAndReportedOnce) {
  int deliveries = 0;
  source_->Start([&](ibmw::transport::SerializedMessage&&) { ++deliveries; });
  ASSERT_NE(driver_.loaned, nullptr);
  const uint64_t value = 42;

  EXPECT_EQ(driver_.DeliverLoan(&value, sizeof(value) - 1), 1);
  EXPECT_EQ(driver_.DeliverLoan(nullptr, sizeof(value)), 1);
  EXPECT_EQ(deliveries, 0);
  EXPECT_EQ(drop_warnings_, 1);

  EXPECT_EQ(driver_.DeliverLoan(&value, sizeof(value)), 1);
  EXPECT_EQ(deliveries, 1);
  source_->Stop();
}

TEST_F(CachedSubscriptionStopTest, LoanOfUnknownSizeTypeIsDropped) {
  type_support_.msg_min_size = 0;
  int deliveries = 0;
  source_->Start([&](ibmw::transport::SerializedMessage&&) { ++deliveries; });
  const uint64_t value = 42;
  EXPECT_EQ(driver_.DeliverLoan(&value, sizeof(value)), 1);
  EXPECT_EQ(deliveries, 0);
  EXPECT_EQ(drop_warnings_, 1);
  source_->Stop();
}

}  // namespace
}  // namespace ibmw::runtime::core::topic_io
