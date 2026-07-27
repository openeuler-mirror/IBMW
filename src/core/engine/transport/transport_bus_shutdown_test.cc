// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/transport/transport_bus.h"

namespace ibmw::runtime::core::transport {
namespace {

struct ShutdownState {
  bool can_shutdown = false;
  bool can_prepare = false;
  int start_calls = 0;
  int prepare_calls = 0;
  int shutdown_calls = 0;
  int destructor_calls = 0;
  std::vector<std::string> events;
};

class ShutdownVetoDriver final : public TransportDriver,
                                 public TransportShutdownCapabilityV1 {
 public:
  explicit ShutdownVetoDriver(std::shared_ptr<ShutdownState> state)
      : state_(std::move(state)) {}
  ~ShutdownVetoDriver() override { ++state_->destructor_calls; }

  std::string_view Name() const noexcept override { return "shutdown-veto"; }
  IBMW_DECLARE_DRIVER_SLOT("shutdown-veto")

  void Initialize(YAML::Node) override {}
  void Start() override { ++state_->start_calls; }
  bool CanShutdownFromCurrentThread() const noexcept override {
    return state_->can_shutdown;
  }
  bool TryPrepareShutdown(std::chrono::milliseconds) noexcept override {
    ++state_->prepare_calls;
    state_->events.emplace_back("prepare");
    return state_->can_prepare;
  }
  void Shutdown() override {
    ++state_->shutdown_calls;
    state_->events.emplace_back("shutdown");
  }
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override {
    return false;
  }
  void Send(MessageFrame&) noexcept override {}

 private:
  std::shared_ptr<ShutdownState> state_;
};

class ThrowingShutdownDriver final : public TransportDriver {
 public:
  ThrowingShutdownDriver(std::string name, bool should_throw,
                         std::vector<std::string>* events)
      : name_(std::move(name)),
        should_throw_(should_throw),
        events_(events) {}

  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("throwing-shutdown")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {
    events_->push_back(name_);
    if (should_throw_) throw std::runtime_error(name_ + " failed");
  }
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override {
    return false;
  }
  void Send(MessageFrame&) noexcept override {}

 private:
  std::string name_;
  bool should_throw_;
  std::vector<std::string>* events_;
};

TEST(TransportBusShutdown, VetoLeavesBusIntactForSuccessfulRetry) {
  auto state = std::make_shared<ShutdownState>();
  TransportBus bus;
  auto concrete_driver = std::make_unique<ShutdownVetoDriver>(state);
  auto* concrete_ptr = concrete_driver.get();
  std::unique_ptr<TransportDriver> driver = std::move(concrete_driver);
  bus.RegisterTransportDriver(
      std::move(driver),
      TransportDriverCapabilitiesV1{
          .shutdown_v1 =
              static_cast<TransportShutdownCapabilityV1*>(concrete_ptr)});
  bus.Initialize(YAML::Load(R"(
drivers:
  - type: shutdown-veto
)"));
  bus.Start();
  ASSERT_EQ(state->start_calls, 1);

  EXPECT_FALSE(bus.CanShutdownFromCurrentThread());
  EXPECT_FALSE(bus.TryShutdown());
  EXPECT_THROW(bus.Shutdown(), std::exception);
  EXPECT_EQ(state->shutdown_calls, 0);
  EXPECT_EQ(state->prepare_calls, 0);
  EXPECT_EQ(state->destructor_calls, 0);

  state->can_shutdown = true;
  EXPECT_TRUE(bus.CanShutdownFromCurrentThread());
  EXPECT_FALSE(bus.TryPrepareShutdown());
  EXPECT_EQ(state->prepare_calls, 1);
  EXPECT_EQ(state->shutdown_calls, 0);
  EXPECT_EQ(state->destructor_calls, 0);

  state->can_prepare = true;
  EXPECT_TRUE(bus.TryPrepareShutdown());
  EXPECT_EQ(state->prepare_calls, 2);
  EXPECT_EQ(state->shutdown_calls, 0);
  EXPECT_EQ(state->destructor_calls, 0);

  EXPECT_TRUE(bus.TryPrepareShutdown());
  EXPECT_EQ(state->prepare_calls, 2);

  EXPECT_TRUE(bus.TryShutdown());
  EXPECT_EQ(state->prepare_calls, 2);
  EXPECT_EQ(state->shutdown_calls, 1);
  EXPECT_EQ(state->destructor_calls, 1);
  EXPECT_EQ(state->events,
            (std::vector<std::string>{"prepare", "prepare", "shutdown"}));

  EXPECT_TRUE(bus.TryShutdown());
  EXPECT_EQ(state->shutdown_calls, 1);
  EXPECT_EQ(state->destructor_calls, 1);
}

TEST(TransportBusShutdown, ThrowingDriverDoesNotSkipLaterDriverOrBusCleanup) {
  std::vector<std::string> events;
  TransportBus bus;
  bus.RegisterTransportDriver(std::make_unique<ThrowingShutdownDriver>(
      "throws", true, &events));
  bus.RegisterTransportDriver(std::make_unique<ThrowingShutdownDriver>(
      "later", false, &events));
  bus.Initialize(YAML::Load(R"(
drivers:
  - type: throws
  - type: later
)"));
  bus.Start();

  EXPECT_TRUE(bus.TryShutdown());
  EXPECT_EQ(events, (std::vector<std::string>{"throws", "later"}));
  EXPECT_EQ(bus.GetState(), ISubsystem::State::kShutdown);
  EXPECT_TRUE(bus.TryShutdown());
  EXPECT_EQ(events, (std::vector<std::string>{"throws", "later"}));
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
