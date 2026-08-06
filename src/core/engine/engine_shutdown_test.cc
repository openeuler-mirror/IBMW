// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/engine.h"

namespace ibmw::runtime::core {
namespace {

using namespace std::chrono_literals;

struct EngineShutdownDriverState {
  std::atomic_bool can_prepare = false;
  std::atomic_int prepare_calls = 0;
  std::atomic_int shutdown_calls = 0;
};

class EngineShutdownDriver final
    : public transport::TransportDriver,
      public transport::TransportShutdownCapabilityV1 {
 public:
  explicit EngineShutdownDriver(
      std::shared_ptr<EngineShutdownDriverState> state)
      : state_(std::move(state)) {}

  std::string_view Name() const noexcept override { return "engine-shutdown"; }
  IBMW_DECLARE_DRIVER_SLOT("engine-shutdown")

  void Initialize(YAML::Node) override {}
  void Start() override {}
  bool CanShutdownFromCurrentThread() const noexcept override { return true; }
  bool TryPrepareShutdown(std::chrono::milliseconds) noexcept override {
    ++state_->prepare_calls;
    return state_->can_prepare.load();
  }
  void Shutdown() override { ++state_->shutdown_calls; }
  bool RegisterSendType(const transport::SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const transport::MessageHandlerWrapper&) noexcept override {
    return false;
  }
  void Send(transport::MessageFrame&) noexcept override {}

 private:
  std::shared_ptr<EngineShutdownDriverState> state_;
};

class ShutdownTestSubsystem final : public ISubsystem {
 public:
  ShutdownTestSubsystem(std::string name, std::vector<std::string>* events,
                        bool throw_on_shutdown)
      : name_(std::move(name)),
        events_(events),
        throw_on_shutdown_(throw_on_shutdown) {}

  void Initialize(YAML::Node) override { state_.store(State::kInit); }
  void Start() override { state_.store(State::kStart); }
  void Shutdown() override {
    events_->push_back(name_);
    if (throw_on_shutdown_) throw std::runtime_error(name_ + " failed");
    state_.store(State::kShutdown);
  }

 private:
  std::string name_;
  std::vector<std::string>* events_;
  bool throw_on_shutdown_;
};

}  // namespace

class EngineShutdownFixture : public ::testing::Test {
 protected:
  static void ConfigureTransportOnly(
      Engine& engine, const std::shared_ptr<EngineShutdownDriverState>& state) {
    auto driver = std::make_unique<EngineShutdownDriver>(state);
    auto* driver_ptr = driver.get();
    engine.transport_bus_.RegisterTransportDriver(
        std::move(driver),
        transport::TransportDriverCapabilitiesV1{
            .shutdown_v1 =
                static_cast<transport::TransportShutdownCapabilityV1*>(
                    driver_ptr)});
    engine.transport_bus_.Initialize(YAML::Load(R"(
drivers:
  - type: engine-shutdown
)"));

    const auto transport_pos = std::find_if(
        engine.subsystems_.begin(), engine.subsystems_.end(),
        [](const auto& entry) { return entry.id == Subsystem::kTransport; });
    ASSERT_NE(transport_pos, engine.subsystems_.end());
    engine.init_order_ = {
        static_cast<size_t>(transport_pos - engine.subsystems_.begin())};
  }

  static void ConfigureSubsystems(Engine& engine, ISubsystem* first,
                                  ISubsystem* second) {
    engine.subsystems_[0].instance = first;
    engine.subsystems_[1].instance = second;
    engine.subsystems_[0].pre_shutdown = {};
    engine.subsystems_[1].pre_shutdown = {};
    engine.init_order_ = {0, 1};
  }

  static bool ImplCompleted(const Engine& engine) {
    return engine.shutdown_impl_flag_.load();
  }
};

TEST_F(EngineShutdownFixture, PublicShutdownRetriesAfterAsyncPrepareVeto) {
  auto state = std::make_shared<EngineShutdownDriverState>();
  Engine engine;
  ConfigureTransportOnly(engine, state);

  int stopping_hook_calls = 0;
  engine.RegisterHook(BeforePhase(Phase::kStopping),
                      [&stopping_hook_calls] { ++stopping_hook_calls; });
  auto shutdown_future = engine.AsyncStart();

  engine.Shutdown();
  ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
  EXPECT_NO_THROW(shutdown_future.get());
  EXPECT_EQ(engine.GetPhase(), Phase::kRunning);
  EXPECT_FALSE(ImplCompleted(engine));
  EXPECT_EQ(stopping_hook_calls, 0);
  EXPECT_EQ(state->prepare_calls.load(), 1);
  EXPECT_EQ(state->shutdown_calls.load(), 0);

  state->can_prepare.store(true);
  EXPECT_NO_THROW(engine.Shutdown());
  EXPECT_EQ(engine.GetPhase(), Phase::kStopped);
  EXPECT_TRUE(ImplCompleted(engine));
  EXPECT_EQ(stopping_hook_calls, 1);
  EXPECT_EQ(state->prepare_calls.load(), 2);
  EXPECT_EQ(state->shutdown_calls.load(), 1);
}

TEST_F(EngineShutdownFixture, AsyncStartDestructorSignalsBeforeJoin) {
  auto state = std::make_shared<EngineShutdownDriverState>();
  std::future<void> shutdown_future;
  const auto begin = std::chrono::steady_clock::now();
  {
    Engine engine;
    ConfigureTransportOnly(engine, state);
    shutdown_future = engine.AsyncStart();
  }

  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  ASSERT_EQ(shutdown_future.wait_for(0s), std::future_status::ready);
  EXPECT_NO_THROW(shutdown_future.get());
  EXPECT_EQ(state->prepare_calls.load(), 2);
  EXPECT_EQ(state->shutdown_calls.load(), 0);
}

TEST_F(EngineShutdownFixture, ThrowingSubsystemDoesNotSkipLaterTeardown) {
  std::vector<std::string> events;
  ShutdownTestSubsystem later("later", &events, false);
  ShutdownTestSubsystem throwing("throwing", &events, true);
  Engine engine;
  ConfigureSubsystems(engine, &later, &throwing);

  auto shutdown_future = engine.AsyncStart();
  engine.Shutdown();

  ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
  EXPECT_NO_THROW(shutdown_future.get());
  EXPECT_EQ(events, (std::vector<std::string>{"throwing", "later"}));
  EXPECT_EQ(engine.GetPhase(), Phase::kStopped);
  EXPECT_TRUE(ImplCompleted(engine));
}

TEST_F(EngineShutdownFixture, StoppingHookCanCallPublicShutdown) {
  auto state = std::make_shared<EngineShutdownDriverState>();
  state->can_prepare.store(true);
  Engine engine;
  ConfigureTransportOnly(engine, state);

  std::atomic_int hook_calls = 0;
  engine.RegisterHook(BeforePhase(Phase::kStopping), [&] {
    ++hook_calls;
    engine.Shutdown();
  });

  auto shutdown_future = engine.AsyncStart();
  engine.Shutdown();

  ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
  EXPECT_NO_THROW(shutdown_future.get());
  EXPECT_EQ(engine.GetPhase(), Phase::kStopped);
  EXPECT_TRUE(ImplCompleted(engine));
  EXPECT_EQ(hook_calls.load(), 1);
  EXPECT_EQ(state->prepare_calls.load(), 1);
  EXPECT_EQ(state->shutdown_calls.load(), 1);
}

}  // namespace ibmw::runtime::core
