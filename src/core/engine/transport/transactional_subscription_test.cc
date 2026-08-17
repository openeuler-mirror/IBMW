// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <future>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cpp/transport/transport_handle.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_driver_dispatcher.h"
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace ibmw::runtime::core::transport {
namespace {

class FakeSubscription final : public LoanedSubscriptionHandle {
 public:
  void Stop() noexcept override { ++stop_calls; }
  bool Close(std::chrono::milliseconds timeout) noexcept override {
    ++close_calls;
    last_close_timeout = timeout;
    return close ? close(timeout) : true;
  }

  int stop_calls = 0;
  int close_calls = 0;
  std::chrono::milliseconds last_close_timeout{0};
  std::function<bool(std::chrono::milliseconds)> close;
};

class ResidualCallbackDriver final : public TransportDriver,
                                     public RetainedLoanCapabilityV1 {
 public:
  ResidualCallbackDriver(std::string name, bool on_message_result,
                          bool attach_result, bool retained_result = true)
      : name_(std::move(name)),
        on_message_result_(on_message_result),
        attach_result_(attach_result),
        retained_result_(retained_result) {}

  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("transactional-subscription-test")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {
    if (message_handler_ != nullptr) {
      shutdown_saw_valid_handler_ =
          !message_handler_->info.msg_type.empty() &&
          !message_handler_->info.topic_name.empty();
    }
  }
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const MessageHandlerWrapper& handler) noexcept override {
    ++on_message_calls_;
    message_handler_ = &handler;
    return on_message_result_;
  }
  bool AttachLoaned(
      const LoanedMessageHandlerWrapper& handler) noexcept override {
    loaned_callback_ = handler.callback;
    return attach_result_;
  }
  std::unique_ptr<LoanedSubscriptionHandle> AttachLoanedRetained(
      const LoanedMessageHandlerWrapper& handler,
      RetainedLoanOptions) noexcept override {
    loaned_callback_ = handler.callback;
    if (!retained_result_) return nullptr;
    auto subscription = std::make_unique<FakeSubscription>();
    subscription_ = subscription.get();
    return subscription;
  }
  void Send(MessageFrame&) noexcept override {}

  void TriggerMessage(int& releases) {
    ASSERT_NE(message_handler_, nullptr);
    TransportInfo info;
    MessageFrame frame{.info = info};
    message_handler_->callback(frame, [&releases] { ++releases; });
  }

  void TriggerLoaned(int& releases) {
    ASSERT_TRUE(loaned_callback_);
    loaned_callback_(nullptr, nullptr, 0, [&releases] { ++releases; });
  }

  FakeSubscription* subscription() const { return subscription_; }
  const MessageHandlerWrapper* message_handler() const {
    return message_handler_;
  }
  int on_message_calls() const { return on_message_calls_; }
  void set_on_message_result(bool result) { on_message_result_ = result; }
  bool shutdown_saw_valid_handler() const {
    return shutdown_saw_valid_handler_;
  }

 private:
  std::string name_;
  bool on_message_result_;
  bool attach_result_;
  bool retained_result_;
  int on_message_calls_ = 0;
  bool shutdown_saw_valid_handler_ = false;
  const MessageHandlerWrapper* message_handler_ = nullptr;
  LoanedMessageHandlerCallback loaned_callback_;
  FakeSubscription* subscription_ = nullptr;
};

ibmw_type_support_base_t MakeTypeSupport() {
  ibmw_type_support_base_t result{};
  result.type_name = [](void*) {
    return ibmw::util::ToIbmwStringView("Frame");
  };
  return result;
}

class TransactionalSubscription : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char* value = std::getenv("IBMW_CORE_METRICS")) saved_mode_ = value;
    unsetenv("IBMW_CORE_METRICS");
#ifdef IBMW_ENABLE_CORE_METRICS
    observability::CoreMetricsController().Initialize(this);
#endif
  }

  void TearDown() override {
#ifdef IBMW_ENABLE_CORE_METRICS
    EXPECT_TRUE(observability::CoreMetricsController().FinalizeSnapshot(this));
#endif
    if (saved_mode_)
      setenv("IBMW_CORE_METRICS", saved_mode_->c_str(), 1);
    else
      unsetenv("IBMW_CORE_METRICS");
  }

 private:
  std::optional<std::string> saved_mode_;
};

TEST_F(TransactionalSubscription, AllDriverFailuresAllowSameKeyRetry) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver accepting("accepting", false, false);
  ResidualCallbackDriver rejecting("rejecting", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetSubTopicsDriversRules(
      {{".*", {"accepting", "rejecting"}}});
  dispatcher.RegisterTransportDriver(&accepting);
  dispatcher.RegisterTransportDriver(&rejecting);
  dispatcher.Initialize();

  int callback_calls = 0;
  MessageHandlerWrapper handler;
  handler.info.msg_type = "Frame";
  handler.info.topic_name = "camera";
  handler.info.pkg_path = "pkg";
  handler.info.module_name = "module";
  handler.callback = [&callback_calls](MessageFrame&,
                                       std::function<void()>&& release) {
    ++callback_calls;
    release();
  };

  EXPECT_FALSE(dispatcher.OnMessage(std::move(handler)));
  EXPECT_EQ(catalog.GetMessageHandlerWrapperPtr(
                "Frame", "camera", "pkg", "module"),
            nullptr);
  EXPECT_EQ(accepting.on_message_calls(), 1);
  EXPECT_EQ(rejecting.on_message_calls(), 1);
  ASSERT_NE(accepting.message_handler(), nullptr);
  EXPECT_EQ(accepting.message_handler()->info.msg_type, "Frame");
  EXPECT_EQ(accepting.message_handler()->info.topic_name, "camera");

  int releases = 0;
  accepting.TriggerMessage(releases);
  EXPECT_EQ(callback_calls, 0);
  EXPECT_EQ(releases, 1);

  accepting.set_on_message_result(true);
  MessageHandlerWrapper retry_handler;
  retry_handler.info.msg_type = "Frame";
  retry_handler.info.topic_name = "camera";
  retry_handler.info.pkg_path = "pkg";
  retry_handler.info.module_name = "module";
  retry_handler.callback = [&callback_calls](
                               MessageFrame&,
                               std::function<void()>&& release) {
    ++callback_calls;
    release();
  };
  EXPECT_TRUE(dispatcher.OnMessage(std::move(retry_handler)));
  EXPECT_EQ(accepting.on_message_calls(), 2);
  EXPECT_EQ(rejecting.on_message_calls(), 2);
  EXPECT_EQ(catalog.GetMessageHandlerWrapperPtr(
                "Frame", "camera", "pkg", "module"),
            accepting.message_handler());
  accepting.TriggerMessage(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 2);
}

TEST_F(TransactionalSubscription, AccessOnMessageFailureDisablesResidualCallback) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver accepting("accepting", false, false);
  ResidualCallbackDriver rejecting("rejecting", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetSubTopicsDriversRules(
      {{".*", {"accepting", "rejecting"}}});
  dispatcher.RegisterTransportDriver(&accepting);
  dispatcher.RegisterTransportDriver(&rejecting);
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::MessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        ibmw_function_base_t*) { ++callback_calls; });
  EXPECT_FALSE(dispatcher.OnMessage(MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));
  EXPECT_EQ(catalog.GetMessageHandlerWrapperPtr(
                "Frame", "camera", "pkg", "module"),
            nullptr);
  ASSERT_NE(accepting.message_handler(), nullptr);
  EXPECT_EQ(accepting.message_handler()->info.msg_type, "Frame");

  int releases = 0;
  accepting.TriggerMessage(releases);
  EXPECT_EQ(callback_calls, 0);
  EXPECT_EQ(releases, 1);

  accepting.set_on_message_result(true);
  EXPECT_TRUE(dispatcher.OnMessage(MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));
  EXPECT_EQ(accepting.on_message_calls(), 2);
  EXPECT_EQ(rejecting.on_message_calls(), 2);
}

TEST_F(TransactionalSubscription, AnySuccessfulOrdinaryDriverKeepsCallbackEnabled) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver rejecting("rejecting", false, false);
  ResidualCallbackDriver accepting("accepting", true, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.SetSubTopicsDriversRules(
      {{".*", {"rejecting", "accepting"}}});
  dispatcher.RegisterTransportDriver(&rejecting);
  dispatcher.RegisterTransportDriver(&accepting);
  dispatcher.Initialize();

  int callback_calls = 0;
  MessageHandlerWrapper handler;
  handler.info.msg_type = "Frame";
  handler.info.topic_name = "camera";
  handler.info.pkg_path = "pkg";
  handler.info.module_name = "module";
  handler.callback = [&callback_calls](MessageFrame&,
                                       std::function<void()>&& release) {
    ++callback_calls;
    release();
  };

  EXPECT_TRUE(dispatcher.OnMessage(std::move(handler)));
  EXPECT_NE(rejecting.message_handler(), accepting.message_handler());
  EXPECT_EQ(catalog.GetMessageHandlerWrapperPtr(
                "Frame", "camera", "pkg", "module"),
            accepting.message_handler());
  MessageHandlerWrapper duplicate;
  duplicate.info.msg_type = "Frame";
  duplicate.info.topic_name = "camera";
  duplicate.info.pkg_path = "pkg";
  duplicate.info.module_name = "module";
  duplicate.callback = [](MessageFrame&, std::function<void()>&& release) {
    release();
  };
  EXPECT_FALSE(dispatcher.OnMessage(std::move(duplicate)));
  EXPECT_EQ(rejecting.on_message_calls(), 1);
  EXPECT_EQ(accepting.on_message_calls(), 1);
  EXPECT_EQ(rejecting.message_handler()->info.msg_type, "Frame");
  EXPECT_EQ(accepting.message_handler()->info.topic_name, "camera");
  int releases = 0;
  rejecting.TriggerMessage(releases);
  accepting.TriggerMessage(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 2);
  dispatcher.Shutdown();
  EXPECT_TRUE(rejecting.shutdown_saw_valid_handler());
  EXPECT_TRUE(accepting.shutdown_saw_valid_handler());
}

TEST_F(TransactionalSubscription, OrdinaryRegistrationWithoutDriversErasesCatalogEntry) {
  TransportCatalog catalog;
  FrameworkAsyncTransportFilterBus filters;
  TransportDriverDispatcher dispatcher;
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&filters);
  dispatcher.Initialize();

  MessageHandlerWrapper handler;
  handler.info.msg_type = "Frame";
  handler.info.topic_name = "camera";
  handler.info.pkg_path = "pkg";
  handler.info.module_name = "module";
  handler.callback = [](MessageFrame&, std::function<void()>&& release) {
    release();
  };

  EXPECT_FALSE(dispatcher.OnMessage(std::move(handler)));
  EXPECT_EQ(catalog.GetMessageHandlerWrapperPtr(
                "Frame", "camera", "pkg", "module"),
            nullptr);
  EXPECT_TRUE(catalog.GetMessageHandlerWrapperMap().empty());
  EXPECT_TRUE(catalog.GetSubTopicIndexMap().empty());
}

TEST_F(TransactionalSubscription, FailedLoanAttachDisablesEveryResidualCallback) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver first("first", false, false);
  ResidualCallbackDriver second("second", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"first", "second"}}});
  dispatcher.RegisterTransportDriver(&first);
  dispatcher.RegisterTransportDriver(&second);
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        size_t, ibmw_function_base_t*) { ++callback_calls; });
  EXPECT_FALSE(dispatcher.AttachLoaned(MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));

  int releases = 0;
  first.TriggerLoaned(releases);
  second.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 0);
  EXPECT_EQ(releases, 2);
}

TEST_F(TransactionalSubscription, LoanedFirstSuccessDisablesOnlyFailedDriverGate) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver first("first", false, false);
  ResidualCallbackDriver second("second", false, true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"first", "second"}}});
  dispatcher.RegisterTransportDriver(&first);
  dispatcher.RegisterTransportDriver(&second);
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        size_t, ibmw_function_base_t*) { ++callback_calls; });
  EXPECT_TRUE(dispatcher.AttachLoaned(MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));

  int releases = 0;
  first.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 0);
  second.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 2);
}

#ifdef IBMW_ENABLE_CORE_METRICS
TEST_F(TransactionalSubscription,
       FrozenMetricsRegistrationStillProbesUntilDriverSuccess) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& metrics = observability::CoreMetricsController();
  metrics.ResetForTesting();
  metrics.Initialize(this);
  metrics.Start(this);

  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver first("first", false, false);
  ResidualCallbackDriver second("second", false, true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"first", "second"}}});
  dispatcher.RegisterTransportDriver(&first);
  dispatcher.RegisterTransportDriver(&second);
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        size_t, ibmw_function_base_t*) { ++callback_calls; });
  EXPECT_TRUE(dispatcher.AttachLoaned(MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));

  int releases = 0;
  first.TriggerLoaned(releases);
  second.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 2);
}
#endif

TEST_F(TransactionalSubscription, RetainedFirstSuccessDisablesRejectedDriverGate) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver first("first", false, false, false);
  ResidualCallbackDriver second("second", false, false, true);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"first", "second"}}});
  dispatcher.RegisterTransportDriver(
      &first, TransportDriverCapabilitiesV1{
                  .retained_loan_v1 = static_cast<RetainedLoanCapabilityV1*>(&first)});
  dispatcher.RegisterTransportDriver(
      &second, TransportDriverCapabilitiesV1{
                   .retained_loan_v1 = static_cast<RetainedLoanCapabilityV1*>(&second)});
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        size_t, ibmw_function_base_t*) { ++callback_calls; });
  auto handle = dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "camera",
          .msg_type_support = &type_support, .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
  ASSERT_NE(handle, nullptr);

  int releases = 0;
  first.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 0);
  second.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 2);
}

TEST_F(TransactionalSubscription, RetainedStopAndCloseDisableResidualCallback) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver driver("retained", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"retained"}}});
  dispatcher.RegisterTransportDriver(
      &driver,
      TransportDriverCapabilitiesV1{
          .retained_loan_v1 =
              static_cast<RetainedLoanCapabilityV1*>(&driver)});
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls](const ibmw_transport_context_base_t*, const void*,
                        size_t, ibmw_function_base_t*) { ++callback_calls; });
  auto handle = dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg",
          .module_name = "module",
          .topic_name = "camera",
          .msg_type_support = &type_support,
          .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
  ASSERT_NE(handle, nullptr);

  int releases = 0;
  driver.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  handle->Stop();
  EXPECT_EQ(driver.subscription()->stop_calls, 1);
  driver.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_TRUE(handle->Close(std::chrono::milliseconds(1)));
  EXPECT_EQ(driver.subscription()->close_calls, 1);
  driver.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(releases, 3);
}

TEST_F(TransactionalSubscription, RetainedCallbackCanStopItsOwnHandle) {
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver driver("retained", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"retained"}}});
  dispatcher.RegisterTransportDriver(
      &driver, TransportDriverCapabilitiesV1{
                   .retained_loan_v1 = static_cast<RetainedLoanCapabilityV1*>(&driver)});
  dispatcher.Initialize();

  auto type_support = MakeTypeSupport();
  LoanedSubscriptionHandle* handle_ptr = nullptr;
  int callback_calls = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callback_calls, &handle_ptr](const ibmw_transport_context_base_t*,
                                     const void*, size_t,
                                     ibmw_function_base_t*) {
        ++callback_calls;
        ASSERT_NE(handle_ptr, nullptr);
        handle_ptr->Stop();
      });
  auto handle = dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "camera",
          .msg_type_support = &type_support, .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
  ASSERT_NE(handle, nullptr);
  handle_ptr = handle.get();

  int releases = 0;
  driver.TriggerLoaned(releases);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_EQ(driver.subscription()->stop_calls, 1);
  EXPECT_EQ(releases, 1);
}

TEST_F(TransactionalSubscription, RetainedStopReturnsWhileCallbackIsBlocked) {
  using namespace std::chrono_literals;
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver driver("retained", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"retained"}}});
  dispatcher.RegisterTransportDriver(
      &driver, TransportDriverCapabilitiesV1{
                   .retained_loan_v1 = static_cast<RetainedLoanCapabilityV1*>(&driver)});
  dispatcher.Initialize();

  std::mutex mutex;
  std::condition_variable state;
  bool entered = false;
  bool release_callback = false;
  auto type_support = MakeTypeSupport();
  ibmw::transport::LoanedMessageCallback callback(
      [&](const ibmw_transport_context_base_t*, const void*, size_t,
          ibmw_function_base_t*) {
        std::unique_lock lock(mutex);
        entered = true;
        state.notify_all();
        state.wait(lock, [&] { return release_callback; });
      });
  auto handle = dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "camera",
          .msg_type_support = &type_support, .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
  ASSERT_NE(handle, nullptr);

  int releases = 0;
  std::thread invoking([&] { driver.TriggerLoaned(releases); });
  {
    std::unique_lock lock(mutex);
    state.wait(lock, [&] { return entered; });
  }
  auto stopped = std::async(std::launch::async, [&] { handle->Stop(); });
  EXPECT_EQ(stopped.wait_for(200ms), std::future_status::ready);
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  state.notify_all();
  stopped.get();
  invoking.join();
  EXPECT_EQ(driver.subscription()->stop_calls, 1);
  EXPECT_EQ(releases, 1);
}

TEST_F(TransactionalSubscription, RetainedCloseUsesInnerTimeoutBudget) {
  using namespace std::chrono_literals;
  TransportCatalog catalog;
  TransportDriverDispatcher dispatcher;
  ResidualCallbackDriver driver("retained", false, false);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"retained"}}});
  dispatcher.RegisterTransportDriver(
      &driver, TransportDriverCapabilitiesV1{
                   .retained_loan_v1 = static_cast<RetainedLoanCapabilityV1*>(&driver)});
  dispatcher.Initialize();

  std::mutex mutex;
  std::condition_variable state;
  bool entered = false;
  bool release_callback = false;
  auto type_support = MakeTypeSupport();
  ibmw::transport::LoanedMessageCallback callback(
      [&](const ibmw_transport_context_base_t*, const void*, size_t,
          ibmw_function_base_t*) {
        std::unique_lock lock(mutex);
        entered = true;
        state.notify_all();
        state.wait(lock, [&] { return release_callback; });
      });
  auto handle = dispatcher.AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper{
          .pkg_path = "pkg", .module_name = "module", .topic_name = "camera",
          .msg_type_support = &type_support, .callback = callback.NativeHandle()},
      RetainedLoanOptions{1});
  ASSERT_NE(handle, nullptr);
  driver.subscription()->close = [](std::chrono::milliseconds timeout) {
    std::this_thread::sleep_for(timeout);
    return false;
  };

  int releases = 0;
  std::thread invoking([&] { driver.TriggerLoaned(releases); });
  {
    std::unique_lock lock(mutex);
    state.wait(lock, [&] { return entered; });
  }
  constexpr auto timeout = 40ms;
  auto closed = std::async(std::launch::async,
                           [&] { return handle->Close(timeout); });
  const auto status = closed.wait_for(250ms);
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  state.notify_all();
  invoking.join();

  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_FALSE(closed.get());
  EXPECT_EQ(driver.subscription()->close_calls, 1);
  EXPECT_EQ(driver.subscription()->last_close_timeout, timeout);
  EXPECT_EQ(releases, 1);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
