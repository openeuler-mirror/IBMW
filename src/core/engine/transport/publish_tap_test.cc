// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/transport/transport_driver_dispatcher.h"
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace ibmw::runtime::core::transport {
namespace {

using namespace std::chrono_literals;

class PublishTapDriver final : public TransportDriver {
 public:
  explicit PublishTapDriver(std::vector<std::string>* events = nullptr)
      : events_(events) {}

  std::string_view Name() const noexcept override { return "publish-tap"; }
  IBMW_DECLARE_DRIVER_SLOT("publish-tap")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override { ++shutdown_calls; }
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return true;
  }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override {
    return false;
  }
  void Send(MessageFrame&) noexcept override {
    ++send_calls;
    if (events_) {
      std::lock_guard lock(events_mutex_);
      events_->emplace_back("driver");
    }
  }

  // Concurrent tests send from several threads.
  std::atomic<int> send_calls{0};
  int shutdown_calls = 0;

 private:
  std::mutex events_mutex_;
  std::vector<std::string>* events_;
};

class PublishTapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dispatcher_.SetTransportCatalog(&catalog_);
    dispatcher_.SetPublishFrameworkAsyncTransportFilterBus(&filters_);
    dispatcher_.SetPubTopicsDriversRules({{".*", {"publish-tap"}}});
    dispatcher_.RegisterTransportDriver(&driver_);
    dispatcher_.Initialize();
    SendTypeWrapper wrapper;
    wrapper.info = info_;
    ASSERT_TRUE(dispatcher_.RegisterSendType(std::move(wrapper)));
    dispatcher_.Start();
  }

  void TearDown() override {
    if (dispatcher_.GetState() != TransportDriverDispatcher::State::kShutdown) {
      dispatcher_.Shutdown();
    }
  }

  void SendOnce() {
    ibmw::transport::Context context;
    dispatcher_.Send(MessageFrame{
        .info = info_,
        .msg_ptr = &payload_,
        .ctx_ref = ibmw::transport::ContextRef(context)});
  }

  TransportCatalog catalog_;
  FrameworkAsyncTransportFilterBus filters_;
  std::vector<std::string> events_;
  PublishTapDriver driver_{&events_};
  TransportDriverDispatcher dispatcher_;
  TransportInfo info_{.msg_type = "TestMessage",
                      .topic_name = "publish_tap_test",
                      .pkg_path = "test",
                      .module_name = "publisher",
                      .index = 1};
  int payload_ = 42;
};

TEST_F(PublishTapTest, SelfUnregisterDoesNotDeadlockAndPreventsReentry) {
  uint64_t handle = 0;
  int calls = 0;
  bool unregistered = false;
  handle = dispatcher_.RegisterPublishTap([&](MessageFrame&) {
    ++calls;
    unregistered = dispatcher_.UnregisterPublishTap(handle);
  });
  ASSERT_NE(handle, 0U);

  auto send = std::async(std::launch::async, [&] { SendOnce(); });
  ASSERT_EQ(send.wait_for(2s), std::future_status::ready);
  send.get();
  EXPECT_TRUE(unregistered);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(driver_.send_calls, 1);

  SendOnce();
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(driver_.send_calls, 2);
}

TEST_F(PublishTapTest, ShutdownFromCallbackDoesNotDeadlockOrSendAfterShutdown) {
  ASSERT_NE(
      dispatcher_.RegisterPublishTap(
          [&](MessageFrame&) { dispatcher_.Shutdown(); }),
      0U);

  auto send = std::async(std::launch::async, [&] { SendOnce(); });
  ASSERT_EQ(send.wait_for(2s), std::future_status::ready);
  send.get();
  EXPECT_EQ(
      dispatcher_.GetState(), TransportDriverDispatcher::State::kShutdown);
  EXPECT_EQ(driver_.shutdown_calls, 1);
  EXPECT_EQ(driver_.send_calls, 0);
}

#ifdef IBMW_ENABLE_CORE_METRICS
class MetricsPublishTapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char* mode = std::getenv("IBMW_CORE_METRICS")) {
      had_metrics_mode_ = true;
      metrics_mode_ = mode;
    }
    ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);

    auto& metrics = observability::CoreMetricsController();
    metrics.ResetForTesting();
    metrics.Initialize(&dispatcher_);
    dispatcher_.SetTransportCatalog(&catalog_);
    dispatcher_.SetPublishFrameworkAsyncTransportFilterBus(&filters_);
    dispatcher_.SetPubTopicsDriversRules({{".*", {"publish-tap"}}});
    dispatcher_.RegisterTransportDriver(&driver_);
    dispatcher_.Initialize();
    SendTypeWrapper wrapper;
    wrapper.info = info_;
    ASSERT_TRUE(dispatcher_.RegisterSendType(std::move(wrapper)));
    metrics.Start(&dispatcher_);
    dispatcher_.Start();

    ASSERT_TRUE(metrics.Enabled());
    const auto* registered = catalog_.GetSendTypeWrapperPtr(
        info_.msg_type, info_.topic_name, info_.pkg_path, info_.module_name);
    ASSERT_NE(registered, nullptr);
    ASSERT_NE(registered->cached_metrics, nullptr);
    ASSERT_NE(registered->cached_metrics_gate, nullptr);
  }

  void TearDown() override {
    if (dispatcher_.GetState() != TransportDriverDispatcher::State::kShutdown) {
      dispatcher_.Shutdown();
    }
    EXPECT_TRUE(
        observability::CoreMetricsController().FinalizeSnapshot(&dispatcher_));
    if (had_metrics_mode_) {
      setenv("IBMW_CORE_METRICS", metrics_mode_.c_str(), 1);
    } else {
      unsetenv("IBMW_CORE_METRICS");
    }
  }

  void RegisterShutdownTap() {
    ASSERT_NE(
        dispatcher_.RegisterPublishTap(
            [&](MessageFrame&) { dispatcher_.Shutdown(); }),
        0U);
  }

  TransportCatalog catalog_;
  FrameworkAsyncTransportFilterBus filters_;
  PublishTapDriver driver_;
  TransportDriverDispatcher dispatcher_;
  TransportInfo info_{.msg_type = "TestMessage",
                      .topic_name = "publish_tap_metrics_test",
                      .pkg_path = "test",
                      .module_name = "publisher",
                      .index = 1};
  int payload_ = 42;
  bool had_metrics_mode_ = false;
  std::string metrics_mode_;
};

TEST_F(MetricsPublishTapTest,
       MessageFrameShutdownFromTapDoesNotDeadlockOrCallDriver) {
  RegisterShutdownTap();

  auto send = std::async(std::launch::async, [&] {
    ibmw::transport::Context context;
    dispatcher_.Send(MessageFrame{
        .info = info_,
        .msg_ptr = &payload_,
        .ctx_ref = ibmw::transport::ContextRef(context)});
  });

  ASSERT_EQ(send.wait_for(2s), std::future_status::ready);
  send.get();
  EXPECT_EQ(
      dispatcher_.GetState(), TransportDriverDispatcher::State::kShutdown);
  EXPECT_EQ(driver_.shutdown_calls, 1);
  EXPECT_EQ(driver_.send_calls, 0);
}

TEST_F(MetricsPublishTapTest,
       SendAccessInfoShutdownFromTapDoesNotDeadlockOrCallDriver) {
  RegisterShutdownTap();

  auto send = std::async(std::launch::async, [&] {
    dispatcher_.Send(SendAccessInfoWrapper{
        .pkg_path = info_.pkg_path,
        .module_name = info_.module_name,
        .topic_name = info_.topic_name,
        .msg_type = ibmw_string_view_t{info_.msg_type.data(),
                                      info_.msg_type.size()},
        .ctx_ptr = nullptr,
        .msg_ptr = &payload_});
  });

  ASSERT_EQ(send.wait_for(2s), std::future_status::ready);
  send.get();
  EXPECT_EQ(
      dispatcher_.GetState(), TransportDriverDispatcher::State::kShutdown);
  EXPECT_EQ(driver_.shutdown_calls, 1);
  EXPECT_EQ(driver_.send_calls, 0);
}
#endif

TEST_F(PublishTapTest, ExternalUnregisterWaitsForInFlightCallback) {
  std::mutex mutex;
  std::condition_variable release_cv;
  std::promise<void> entered;
  bool release = false;
  const auto handle = dispatcher_.RegisterPublishTap([&](MessageFrame&) {
    entered.set_value();
    std::unique_lock lock(mutex);
    release_cv.wait(lock, [&] { return release; });
  });
  ASSERT_NE(handle, 0U);

  auto send = std::async(std::launch::async, [&] { SendOnce(); });
  ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
  auto unregister = std::async(
      std::launch::async,
      [&] { return dispatcher_.UnregisterPublishTap(handle); });
  EXPECT_EQ(unregister.wait_for(50ms), std::future_status::timeout);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  release_cv.notify_one();

  ASSERT_EQ(unregister.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(unregister.get());
  ASSERT_EQ(send.wait_for(2s), std::future_status::ready);
  send.get();
}

TEST_F(PublishTapTest, ConcurrentUnregisterWaitsForAllInFlightCallbacks) {
  std::mutex mutex;
  std::condition_variable cv;
  std::promise<void> both_entered;
  std::atomic<int> entered{0};
  int released = 0;
  const auto handle = dispatcher_.RegisterPublishTap([&](MessageFrame&) {
    const int ordinal = entered.fetch_add(1) + 1;
    if (ordinal == 2) both_entered.set_value();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return released >= ordinal; });
  });
  ASSERT_NE(handle, 0U);

  auto first = std::async(std::launch::async, [&] { SendOnce(); });
  auto second = std::async(std::launch::async, [&] { SendOnce(); });
  ASSERT_EQ(both_entered.get_future().wait_for(2s), std::future_status::ready);

  auto unregister = std::async(std::launch::async, [&] {
    return dispatcher_.UnregisterPublishTap(handle);
  });
  EXPECT_EQ(unregister.wait_for(50ms), std::future_status::timeout);
  {
    std::lock_guard lock(mutex);
    released = 1;
  }
  cv.notify_all();
  EXPECT_EQ(unregister.wait_for(2s), std::future_status::timeout);
  {
    std::lock_guard lock(mutex);
    released = 2;
  }
  cv.notify_all();
  ASSERT_EQ(unregister.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(unregister.get());
  ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
  ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
  first.get();
  second.get();
}

TEST_F(PublishTapTest, RunsInRegistrationOrderAndExceptionsAreFailOpen) {
  const auto first = dispatcher_.RegisterPublishTap(
      [&](MessageFrame&) { events_.emplace_back("first"); });
  const auto second = dispatcher_.RegisterPublishTap([&](MessageFrame&) {
    events_.emplace_back("second");
    throw std::runtime_error("expected test failure");
  });
  ASSERT_NE(first, 0U);
  ASSERT_NE(second, 0U);

  SendOnce();
  EXPECT_EQ(events_,
            (std::vector<std::string>{"first", "second", "driver"}));
  EXPECT_EQ(driver_.send_calls, 1);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
