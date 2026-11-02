// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

  void Initialize(YAML::Node) override {
    if (throw_on_init_) {
      events_->push_back(name_ + ".init");
      throw std::runtime_error(name_ + " init failed");
    }
    state_.store(State::kInit);
  }
  void Start() override { state_.store(State::kStart); }
  void ThrowOnInit() { throw_on_init_ = true; }
  void Shutdown() override {
    events_->push_back(name_);
    if (throw_on_shutdown_) throw std::runtime_error(name_ + " failed");
    state_.store(State::kShutdown);
  }

 private:
  std::string name_;
  std::vector<std::string>* events_;
  bool throw_on_shutdown_;
  bool throw_on_init_ = false;
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
    engine.shutdown_order_ = engine.init_order_;
  }

  static void ConfigureSubsystems(Engine& engine, ISubsystem* first,
                                  ISubsystem* second) {
    engine.subsystems_[0].instance = first;
    engine.subsystems_[1].instance = second;
    engine.subsystems_[0].pre_shutdown = {};
    engine.subsystems_[1].pre_shutdown = {};
    engine.init_order_ = {0, 1};
    engine.shutdown_order_ = {1, 0};
  }

  static size_t IndexOf(const Engine& engine, Subsystem id) {
    const auto it = std::find_if(engine.subsystems_.begin(), engine.subsystems_.end(),
                                 [id](const auto& entry) { return entry.id == id; });
    return static_cast<size_t>(it - engine.subsystems_.begin());
  }

  static size_t StopRank(const Engine& engine, Subsystem id) {
    const auto idx = IndexOf(engine, id);
    return static_cast<size_t>(
        std::find(engine.shutdown_order_.begin(), engine.shutdown_order_.end(), idx) -
        engine.shutdown_order_.begin());
  }

  static Engine::SubsystemEntry& Entry(Engine& engine, Subsystem id) {
    return engine.subsystems_[IndexOf(engine, id)];
  }

  static void SetInitOrder(Engine& engine, std::vector<size_t> order) {
    engine.init_order_ = order;
    engine.shutdown_order_.assign(order.rbegin(), order.rend());
  }

  static size_t ShutdownOrderSize(const Engine& engine) { return engine.shutdown_order_.size(); }

  static size_t HookCount(const Engine& engine, HookPoint point) {
    return engine.SnapshotHooks(point).size();
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

// CORE-025: extensions initialize right after the configurator, but must stop
// after everything that uses them and before the logger / executors.
TEST_F(EngineShutdownFixture, ExtensionStopsBeforeLoggerAndExecutors) {
  Engine engine;
  const auto ext = StopRank(engine, Subsystem::kExtension);
  EXPECT_LT(StopRank(engine, Subsystem::kModule), ext);
  EXPECT_LT(StopRank(engine, Subsystem::kTransport), ext);
  EXPECT_LT(StopRank(engine, Subsystem::kService), ext);
  EXPECT_LT(ext, StopRank(engine, Subsystem::kLogger));
  EXPECT_LT(ext, StopRank(engine, Subsystem::kExecutor));
  EXPECT_LT(ext, StopRank(engine, Subsystem::kMainThread));
  EXPECT_LT(ext, StopRank(engine, Subsystem::kWatchdog));
  EXPECT_EQ(StopRank(engine, Subsystem::kConfigurator), ShutdownOrderSize(engine) - 1);
}

// CORE-011: the subsystem whose Initialize() throws is rolled back too, in
// shutdown order, and the engine ends up stopped.
TEST_F(EngineShutdownFixture, InitFailureRollsBackFailingSubsystem) {
  std::vector<std::string> events;
  ShutdownTestSubsystem ok("ok", &events, false);
  ShutdownTestSubsystem bad("bad", &events, false);
  bad.ThrowOnInit();
  Engine engine;
  auto& ok_entry = Entry(engine, Subsystem::kMainThread);
  auto& bad_entry = Entry(engine, Subsystem::kWatchdog);
  ok_entry.instance = &ok;
  bad_entry.instance = &bad;
  SetInitOrder(engine, {IndexOf(engine, Subsystem::kConfigurator),
                        IndexOf(engine, Subsystem::kMainThread),
                        IndexOf(engine, Subsystem::kWatchdog)});

  EXPECT_ANY_THROW(engine.Initialize({.config_node = YAML::Load("ibmw: {}")}));
  EXPECT_EQ(events, (std::vector<std::string>{"bad.init", "bad", "ok"}));
  EXPECT_EQ(engine.GetPhase(), Phase::kStopped);
}

// CORE-013: the rollback is the only teardown after a failed Initialize; the
// destructor and Shutdown() do not run every subsystem's Shutdown() again.
TEST_F(EngineShutdownFixture, NoSecondTeardownAfterInitRollback) {
  std::vector<std::string> events;
  ShutdownTestSubsystem ok("ok", &events, false);
  ShutdownTestSubsystem bad("bad", &events, false);
  ShutdownTestSubsystem never("never", &events, false);
  bad.ThrowOnInit();
  int stopping_hooks = 0;
  {
    Engine engine;
    Entry(engine, Subsystem::kMainThread).instance = &ok;
    Entry(engine, Subsystem::kWatchdog).instance = &bad;
    Entry(engine, Subsystem::kExecutor).instance = &never;
    SetInitOrder(engine, {IndexOf(engine, Subsystem::kConfigurator),
                          IndexOf(engine, Subsystem::kMainThread),
                          IndexOf(engine, Subsystem::kWatchdog),
                          IndexOf(engine, Subsystem::kExecutor)});
    engine.RegisterHook(BeforePhase(Phase::kStopping), [&] { ++stopping_hooks; });

    EXPECT_ANY_THROW(engine.Initialize({.config_node = YAML::Load("ibmw: {}")}));
    engine.Shutdown();
    engine.Shutdown();
  }
  EXPECT_EQ(events, (std::vector<std::string>{"bad.init", "bad", "ok"}));
  EXPECT_EQ(stopping_hooks, 0);
}

TEST_F(EngineShutdownFixture, NeverInitializedEngineHasNothingToTearDown) {
  std::vector<std::string> events;
  ShutdownTestSubsystem never("never", &events, false);
  int stopping_hooks = 0;
  {
    Engine engine;
    Entry(engine, Subsystem::kMainThread).instance = &never;
    engine.RegisterHook(BeforePhase(Phase::kStopping), [&] { ++stopping_hooks; });
  }
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(stopping_hooks, 0);
}

// CORE-010: hooks may be registered (e.g. by extensions) while another thread
// snapshots the same hook point.
TEST_F(EngineShutdownFixture, ConcurrentHookRegistrationIsSafe) {
  Engine engine;
  const auto point = BeforePhase(Phase::kStart);
  std::atomic_bool stop = false;
  std::thread reader([&] {
    while (!stop.load()) (void)HookCount(engine, point);
  });
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&] {
      for (int i = 0; i < 1000; ++i) engine.RegisterHook(point, [] {});
    });
  }
  for (auto& w : writers) w.join();
  stop = true;
  reader.join();
  EXPECT_EQ(HookCount(engine, point), 4000u);
}

}  // namespace ibmw::runtime::core
