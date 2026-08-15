#include "observability/core_metrics.h"
#include "cpp/transport/transport_handle.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "engine/transport/transport_driver.h"

#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include <gtest/gtest.h>

namespace ibmw::runtime::core::observability {
class CoreMetrics : public ::testing::Test {
 protected:
  void TearDown() override {
    unsetenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
    unsetenv("IBMW_CORE_METRICS");
    Controller::Instance().ResetForTesting();
  }
};

class ReceiveMetricsDriver final : public transport::TransportDriver {
 public:
  std::string_view Name() const noexcept override { return "receive-driver"; }
  IBMW_DECLARE_DRIVER_SLOT("receive-metrics-test")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const transport::SendTypeWrapper&) noexcept override {
    return false;
  }
  bool OnMessage(const transport::MessageHandlerWrapper&) noexcept override {
    return false;
  }
  bool AttachLoaned(
      const transport::LoanedMessageHandlerWrapper& handler) noexcept override {
    callback_ = handler.callback;
    return true;
  }
  void Send(transport::MessageFrame&) noexcept override {}

  void Trigger(int& releases) {
    callback_(nullptr, nullptr, 0, [&releases] { ++releases; });
  }

 private:
  transport::LoanedMessageHandlerCallback callback_;
};

class ConcurrentReceiveGate final : public LoanedReceiveGate {
 public:
  void Invoke(const std::function<void()>& callback,
              const std::function<void()>& release) {
    {
      std::lock_guard lock(mutex_);
      if (!enabled_) {
        release();
        return;
      }
      ++active_;
    }
    Invocation invocation{this, current_};
    current_ = &invocation;
    callback();
    current_ = invocation.previous;
    release();
    {
      std::lock_guard lock(mutex_);
      --active_;
      drained_.notify_all();
    }
  }

  void Disable() noexcept override {
    BeginDisable();
    WaitDisabled();
  }

  void BeginDisable() noexcept override {
    std::unique_lock lock(mutex_);
    enabled_ = false;
  }

  void WaitDisabled() noexcept override {
    std::unique_lock lock(mutex_);
    const size_t own_depth = CurrentDepth();
    ++waiters_;
    waiter_entered_.notify_all();
    drained_.wait(lock, [this, own_depth] { return active_ <= own_depth; });
    --waiters_;
  }

  bool IsCurrentInvocation() const noexcept override {
    return CurrentDepth() != 0;
  }

  void WaitForWaiters(size_t count) {
    std::unique_lock lock(mutex_);
    waiter_entered_.wait(lock, [this, count] { return waiters_ >= count; });
  }

 private:
  struct Invocation {
    ConcurrentReceiveGate* gate;
    Invocation* previous;
  };
  size_t CurrentDepth() const noexcept {
    size_t depth = 0;
    for (auto* invocation = current_; invocation;
         invocation = invocation->previous)
      if (invocation->gate == this) ++depth;
    return depth;
  }
  static thread_local Invocation* current_;
  std::mutex mutex_;
  std::condition_variable drained_;
  std::condition_variable waiter_entered_;
  bool enabled_ = true;
  size_t active_ = 0;
  size_t waiters_ = 0;
};

thread_local ConcurrentReceiveGate::Invocation* ConcurrentReceiveGate::current_ =
    nullptr;

class TwoPartyBarrier {
 public:
  void ArriveAndWait() {
    std::unique_lock lock(mutex_);
    if (++arrived_ == 2) {
      ready_.notify_all();
      return;
    }
    ready_.wait(lock, [this] { return arrived_ == 2; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  size_t arrived_ = 0;
};

TEST_F(CoreMetrics, OffModeDoesNotCreateOutput) {
  Runtime runtime(ObservationMode::kOff);
  EXPECT_TRUE(runtime.FinalizeSnapshot());
  EXPECT_TRUE(runtime.registry().SnapshotJsonl().empty());
}

TEST_F(CoreMetrics, FreezeKeepsStableConcurrentSlots) {
  Registry registry;
  auto* logical = registry.RegisterLogicalSlot("topic", "type");
  auto* driver = registry.RegisterDriverSlot("topic", "type", "driver");
  ASSERT_NE(logical, nullptr);
  ASSERT_NE(driver, nullptr);
  registry.Freeze();
  EXPECT_EQ(registry.RegisterLogicalSlot("later", "type"), nullptr);
  std::thread a([&] { for (int i = 0; i < 1000; ++i) registry.AddLogicalAttempt(logical); });
  std::thread b([&] { for (int i = 0; i < 1000; ++i) { registry.AddSendAttempt(driver); registry.AddSendReturned(driver); } });
  a.join(); b.join();
  const auto json = registry.SnapshotJsonl();
  EXPECT_NE(json.find("\"attempted\":1000"), std::string::npos);
  EXPECT_NE(json.find("\"returned\":1000"), std::string::npos);
}

TEST_F(CoreMetrics, LoanCountersHaveAuditableJsonlKinds) {
  // This slice covers catalog-based user loans only. POD loans deliberately
  // bypass these slots and must not be represented by fabricated counters.
  Registry registry;
  auto* logical = registry.RegisterLogicalSlot("topic", "type");
  auto* driver = registry.RegisterLoanDriverSlot("source");
  ASSERT_NE(logical, nullptr);
  ASSERT_NE(driver, nullptr);
  registry.AddLoanAttempt(logical);
  registry.AddLoanSucceeded(logical);
  registry.AddLoanAttempt(logical);
  registry.AddLoanFailed(logical);
  registry.AddLoanPublishCalled(driver);
  registry.AddLoanReleaseCalled(driver);
  registry.Freeze();

  const auto json = registry.SnapshotJsonl();
  EXPECT_NE(json.find("\"kind\":\"logical_loan\""), std::string::npos);
  EXPECT_NE(json.find("\"loan_attempted\":2"), std::string::npos);
  EXPECT_NE(json.find("\"loan_succeeded\":1"), std::string::npos);
  EXPECT_NE(json.find("\"loan_failed\":1"), std::string::npos);
  EXPECT_NE(json.find("\"kind\":\"source_driver_loan\""), std::string::npos);
  EXPECT_NE(json.find("\"driver\":\"source\""), std::string::npos);
  EXPECT_NE(json.find("\"loan_publish_called\":1"), std::string::npos);
  EXPECT_NE(json.find("\"loan_release_called\":1"), std::string::npos);
  EXPECT_EQ(registry.RegisterLoanDriverSlot("late"), nullptr);
}

TEST_F(CoreMetrics, FrameworkJsonlIncludesFullRegistrationIdentity) {
  Registry registry;
  ASSERT_NE(registry.RegisterLogicalSlot("topic", "type", "pkg", "module"),
            nullptr);
  ASSERT_NE(registry.RegisterDriverSlot("topic", "type", "driver", "pkg",
                                        "module"),
            nullptr);

  const auto json = registry.SnapshotJsonl();
  EXPECT_NE(json.find(
                "\"kind\":\"logical_publish\",\"cumulative\":{\"pkg\":\"pkg\","
                "\"module\":\"module\",\"topic\":\"topic\",\"type\":\"type\""),
            std::string::npos);
  EXPECT_NE(json.find(
                "\"kind\":\"driver_send\",\"cumulative\":{\"pkg\":\"pkg\","
                "\"module\":\"module\",\"topic\":\"topic\",\"type\":\"type\","
                "\"driver\":\"driver\""),
            std::string::npos);
}

TEST_F(CoreMetrics, PodLoanCountersAreIndependentAndDriverScoped) {
  Registry registry;
  auto* logical = registry.RegisterPodLogicalSlot("camera", "Frame");
  auto* selected = registry.RegisterPodDriverSlot("camera", "Frame", "selected");
  auto* other = registry.RegisterPodDriverSlot("camera", "Frame", "other");
  ASSERT_NE(logical, nullptr);
  ASSERT_NE(selected, nullptr);
  ASSERT_NE(other, nullptr);
  registry.AddPodLoanAttempt(logical);
  registry.AddPodLoanSucceeded(logical);
  registry.AddPodLoanAttempt(logical);
  registry.AddPodLoanFailed(logical);
  registry.AddPodLoanPublishCalled(selected);
  registry.AddPodLoanReleaseCalled(selected);
  registry.Freeze();

  const auto json = registry.SnapshotJsonl();
  EXPECT_NE(json.find("\"kind\":\"logical_pod_loan\""), std::string::npos);
  EXPECT_NE(json.find("\"kind\":\"source_driver_pod_loan\""), std::string::npos);
  EXPECT_NE(json.find("\"topic\":\"camera\",\"type\":\"Frame\",\"driver\":\"selected\",\"loan_publish_called\":1,\"loan_release_called\":1"), std::string::npos);
  EXPECT_NE(json.find("\"driver\":\"other\",\"loan_publish_called\":0,\"loan_release_called\":0"), std::string::npos);
  EXPECT_EQ(json.find("\"kind\":\"logical_loan\""), std::string::npos);
  EXPECT_EQ(registry.RegisterPodLogicalSlot("late", "Frame"), nullptr);
}

TEST_F(CoreMetrics, DispatcherReceiveUpdatesAuditableCounters) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  transport::TransportCatalog catalog;
  transport::TransportDriverDispatcher dispatcher;
  ReceiveMetricsDriver driver;
  auto& controller = Controller::Instance();
  controller.Initialize(&dispatcher);
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetSubTopicsDriversRules({{".*", {"receive-driver"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();

  ibmw_type_support_base_t type_support{};
  type_support.type_name = [](void*) {
    return ibmw::util::ToIbmwStringView("Frame");
  };
  int callbacks = 0;
  ibmw::transport::LoanedMessageCallback callback(
      [&callbacks](const ibmw_transport_context_base_t*, const void*, size_t,
                   ibmw_function_base_t*) { ++callbacks; });
  ASSERT_TRUE(dispatcher.AttachLoaned(transport::MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "module",
      .topic_name = "camera",
      .msg_type_support = &type_support,
      .callback = callback.NativeHandle()}));

  int releases = 0;
  driver.Trigger(releases);
  EXPECT_EQ(callbacks, 1);
  EXPECT_EQ(releases, 1);
  const auto json = controller.registry().SnapshotJsonl();
  EXPECT_NE(json.find("\"kind\":\"loaned_receive_callback\""),
             std::string::npos);
  EXPECT_NE(json.find("\"topic\":\"camera\",\"type\":\"Frame\",\"driver\":\"receive-driver\""),
             std::string::npos);
  EXPECT_NE(json.find("\"callback_entered\":1"), std::string::npos);
  EXPECT_NE(json.find("\"callback_completed\":1"), std::string::npos);
  EXPECT_NE(json.find("\"callback_threw_std\":0"), std::string::npos);
  EXPECT_NE(json.find("\"callback_threw_nonstd\":0"), std::string::npos);
  EXPECT_NE(json.find("\"release_called\":1"), std::string::npos);
  EXPECT_TRUE(controller.FinalizeSnapshot(&dispatcher));
}

TEST_F(CoreMetrics, ConcurrentSelfShutdownDrainsDetachedGatesWithoutDeadlock) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* lifecycle = reinterpret_cast<const void*>(0x4300);
  const void* dispatcher = reinterpret_cast<const void*>(0x4301);
  const void* registration_a = reinterpret_cast<const void*>(0x4302);
  const void* registration_b = reinterpret_cast<const void*>(0x4303);
  controller.Initialize(lifecycle);

  auto gate_a = std::make_shared<ConcurrentReceiveGate>();
  auto gate_b = std::make_shared<ConcurrentReceiveGate>();
  ASSERT_NE(controller.RegisterLoanedReceive(dispatcher, registration_a,
                                              "a", "Frame", "driver", gate_a),
            nullptr);
  ASSERT_NE(controller.RegisterLoanedReceive(dispatcher, registration_b,
                                              "b", "Frame", "driver", gate_b),
            nullptr);

  TwoPartyBarrier callbacks_entered;
  int callbacks_a = 0;
  int callbacks_b = 0;
  int releases_a = 0;
  int releases_b = 0;
  std::thread a([&] {
    gate_a->Invoke(
        [&] {
          ++callbacks_a;
          callbacks_entered.ArriveAndWait();
          controller.DisableLoanedReceive(dispatcher);
        },
        [&] { ++releases_a; });
  });
  std::thread b([&] {
    gate_b->Invoke(
        [&] {
          ++callbacks_b;
          callbacks_entered.ArriveAndWait();
          controller.DisableLoanedReceive(dispatcher);
        },
        [&] { ++releases_b; });
  });
  a.join();
  b.join();

  EXPECT_EQ(callbacks_a, 1);
  EXPECT_EQ(callbacks_b, 1);
  EXPECT_EQ(releases_a, 1);
  EXPECT_EQ(releases_b, 1);
  gate_a->Invoke([&] { ++callbacks_a; }, [&] { ++releases_a; });
  gate_b->Invoke([&] { ++callbacks_b; }, [&] { ++releases_b; });
  EXPECT_EQ(callbacks_a, 1);
  EXPECT_EQ(callbacks_b, 1);
  EXPECT_EQ(releases_a, 2);
  EXPECT_EQ(releases_b, 2);
  EXPECT_EQ(controller.RegisterLoanedReceive(dispatcher, registration_a,
                                              "late", "Frame", "driver", gate_a),
            nullptr);
}

TEST_F(CoreMetrics, SelfShutdownWaitsForOtherInvocationButNotItself) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* lifecycle = reinterpret_cast<const void*>(0x4310);
  const void* dispatcher = reinterpret_cast<const void*>(0x4311);
  const void* registration = reinterpret_cast<const void*>(0x4312);
  controller.Initialize(lifecycle);
  auto gate = std::make_shared<ConcurrentReceiveGate>();
  ASSERT_NE(controller.RegisterLoanedReceive(dispatcher, registration, "same",
                                              "Frame", "driver", gate),
            nullptr);

  TwoPartyBarrier callbacks_entered;
  std::atomic_bool disable_returned{false};
  std::atomic_int callbacks{0};
  std::atomic_int releases{0};
  std::thread stopper([&] {
    gate->Invoke(
        [&] {
          ++callbacks;
          callbacks_entered.ArriveAndWait();
          controller.DisableLoanedReceive(dispatcher);
          disable_returned.store(true, std::memory_order_release);
        },
        [&] { ++releases; });
  });
  std::thread other([&] {
    gate->Invoke(
        [&] {
          ++callbacks;
          callbacks_entered.ArriveAndWait();
          gate->WaitForWaiters(1);
          EXPECT_FALSE(disable_returned.load(std::memory_order_acquire));
        },
        [&] { ++releases; });
  });
  stopper.join();
  other.join();

  EXPECT_TRUE(disable_returned.load(std::memory_order_acquire));
  EXPECT_EQ(callbacks.load(), 2);
  EXPECT_EQ(releases.load(), 2);
  gate->Invoke([&] { ++callbacks; }, [&] { ++releases; });
  EXPECT_EQ(callbacks.load(), 2);
  EXPECT_EQ(releases.load(), 3);
}

TEST_F(CoreMetrics, ConcurrentExternalShutdownsBothWaitForCallbackExit) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* lifecycle = reinterpret_cast<const void*>(0x4320);
  const void* dispatcher = reinterpret_cast<const void*>(0x4321);
  const void* registration = reinterpret_cast<const void*>(0x4322);
  controller.Initialize(lifecycle);
  auto gate = std::make_shared<ConcurrentReceiveGate>();
  ASSERT_NE(controller.RegisterLoanedReceive(dispatcher, registration,
                                              "external", "Frame", "driver",
                                              gate),
            nullptr);

  std::mutex callback_mutex;
  std::condition_variable callback_state;
  bool callback_entered = false;
  bool permit_callback_exit = false;
  std::atomic_int callbacks{0};
  std::atomic_int releases{0};
  std::atomic_int disables_returned{0};
  TwoPartyBarrier second_started;
  std::thread callback([&] {
    gate->Invoke(
        [&] {
          ++callbacks;
          std::unique_lock lock(callback_mutex);
          callback_entered = true;
          callback_state.notify_all();
          callback_state.wait(lock, [&] { return permit_callback_exit; });
        },
        [&] { ++releases; });
  });
  {
    std::unique_lock lock(callback_mutex);
    callback_state.wait(lock, [&] { return callback_entered; });
  }
  std::thread first([&] {
    controller.DisableLoanedReceive(dispatcher);
    ++disables_returned;
  });
  std::thread second([&] {
    second_started.ArriveAndWait();
    controller.DisableLoanedReceive(dispatcher);
    ++disables_returned;
  });
  second_started.ArriveAndWait();
  gate->WaitForWaiters(1);
  EXPECT_EQ(disables_returned.load(), 0);
  {
    std::lock_guard lock(callback_mutex);
    permit_callback_exit = true;
  }
  callback_state.notify_all();
  callback.join();
  first.join();
  second.join();

  EXPECT_EQ(disables_returned.load(), 2);
  EXPECT_EQ(callbacks.load(), 1);
  EXPECT_EQ(releases.load(), 1);
  gate->Invoke([&] { ++callbacks; }, [&] { ++releases; });
  EXPECT_EQ(callbacks.load(), 1);
  EXPECT_EQ(releases.load(), 2);
}

TEST_F(CoreMetrics, PodSidecarSeparatesDispatcherRegistrationAndDriver) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* lifecycle = reinterpret_cast<const void*>(0x4100);
  const void* dispatcher_a = reinterpret_cast<const void*>(0x4101);
  const void* dispatcher_b = reinterpret_cast<const void*>(0x4102);
  const void* registration_a = reinterpret_cast<const void*>(0x4201);
  const void* registration_b = reinterpret_cast<const void*>(0x4202);
  controller.Initialize(lifecycle);

  class NamedDriver final : public transport::TransportDriver {
   public:
    std::string_view Name() const noexcept override { return "shared"; }
    ibmw_driver_slot_t Slot() const noexcept override {
      return ::ibmw::DriverSlotFromString("shared");
    }
    void Initialize(YAML::Node) override {}
    void Start() override {}
    void Shutdown() override {}
    bool RegisterSendType(const transport::SendTypeWrapper&) noexcept override { return false; }
    bool OnMessage(const transport::MessageHandlerWrapper&) noexcept override { return false; }
    void Send(transport::MessageFrame&) noexcept override {}
  } driver;

  ASSERT_TRUE(controller.RegisterPod(dispatcher_a, registration_a,
                                     "pod-a", "Frame", &driver));
  ASSERT_TRUE(controller.RegisterPod(dispatcher_b, registration_b,
                                     "pod-b", "Frame", &driver));
  EXPECT_NE(controller.FindPod(dispatcher_a, registration_a), nullptr);
  EXPECT_EQ(controller.FindPod(dispatcher_a, registration_b), nullptr);
  EXPECT_NE(controller.FindPod(dispatcher_b, registration_b), nullptr);

  controller.UnregisterPod(dispatcher_a, registration_a);
  EXPECT_EQ(controller.FindPod(dispatcher_a, registration_a), nullptr);
  EXPECT_NE(controller.FindPod(dispatcher_b, registration_b), nullptr);
  controller.Start(lifecycle);
  EXPECT_TRUE(controller.RegisterPod(dispatcher_a, registration_a,
                                      "late", "Frame", &driver));
  EXPECT_TRUE(controller.FinalizeSnapshot(lifecycle));
  unsetenv("IBMW_CORE_METRICS");
}

TEST_F(CoreMetrics, WritesCompleteJsonl) {
  const auto dir = std::filesystem::temp_directory_path() / "ibmw_core_metrics_test";
  std::filesystem::remove_all(dir);
  Registry registry;
  registry.RegisterLogicalSlot("topic", "type");
  registry.Freeze();
  ASSERT_TRUE(registry.WriteSnapshot(dir.string()));
  std::ifstream input(dir / "metrics.jsonl");
  std::string line;
  ASSERT_TRUE(std::getline(input, line));
  EXPECT_NE(line.find("\"schema\":\"ibmw.core_metrics.v1\""), std::string::npos);
  EXPECT_NE(line.find("\"snapshot_seq\":1"), std::string::npos);
  EXPECT_NE(line.find("\"kind\":"), std::string::npos);
  EXPECT_NE(line.find("\"cumulative\":"), std::string::npos);
  EXPECT_NE(line.find("\"complete\":true"), std::string::npos);
  std::filesystem::remove_all(dir);
}

#ifdef IBMW_ENABLE_REPORTER_V1
TEST_F(CoreMetrics, CoreAndReporterSnapshotSequencesAreContiguous) {
  Registry registry;
  ASSERT_NE(registry.RegisterLogicalSlot("camera", "Frame"), nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"extension_metric", 16};
  descriptor.metric = {"count", 5};
  descriptor.topic = {"camera", 6};
  descriptor.type = {"Frame", 5};
  descriptor.driver = {"iox", 3};
  auto* reporter = registry.RegisterReporterSlot(&descriptor);
  ASSERT_NE(reporter, nullptr);
  reporter->value.store(7);

  const auto json = registry.SnapshotJsonl();
  const auto first = json.find("\"snapshot_seq\":1");
  const auto second = json.find("\"snapshot_seq\":2");
  const auto third = json.find("\"snapshot_seq\":3");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(second, std::string::npos);
  ASSERT_NE(third, std::string::npos);
  EXPECT_LT(first, second);
  EXPECT_LT(second, third);
  EXPECT_EQ(json.find("\"snapshot_seq\":4"), std::string::npos);
  EXPECT_EQ(json.find("\"snapshot_seq\":1000000"), std::string::npos);
  EXPECT_NE(json.find(
                "\"kind\":\"extension_metric\",\"metric\":\"count\","
                "\"topic\":\"camera\",\"type\":\"Frame\","
                "\"driver\":\"iox\",\"value\":7,\"cumulative\":"),
            std::string::npos);
}
#endif

TEST_F(CoreMetrics, EscapesJsonStrings) {
  Registry registry;
  registry.RegisterLogicalSlot("a\"\\\n\t", "type\r");
  const auto json = registry.SnapshotJsonl();
  EXPECT_NE(json.find("a\\\"\\\\\\n\\t"), std::string::npos);
  EXPECT_NE(json.find("type\\r"), std::string::npos);
}

TEST_F(CoreMetrics, ControllerOwnsFullFrameworkIdentityAndFreezesFailOpen) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  controller.Initialize();
  const auto* dispatcher = reinterpret_cast<const void*>(0x1234);
  const auto* wrapper = reinterpret_cast<const transport::SendTypeWrapper*>(0x5678);
  std::vector<transport::TransportDriver*> drivers;
  std::string topic = "temporary-topic";
  std::string type = "temporary-type";
  const auto* wrapper2 = reinterpret_cast<const transport::SendTypeWrapper*>(0x5679);
  ASSERT_TRUE(controller.Register(dispatcher, wrapper, "pkg-a", "module-a",
                                  topic, type, drivers));
  EXPECT_FALSE(controller.Register(dispatcher, wrapper, "pkg-a", "module-a",
                                   topic, type, drivers));
  EXPECT_TRUE(controller.Register(dispatcher, wrapper2, "pkg-b", "module-b",
                                  topic, type, drivers));
  topic.assign("overwritten");
  type.assign("overwritten");
  auto* first_identity = controller.Find(dispatcher, "pkg-a", "module-a",
                                         "temporary-topic", "temporary-type");
  auto* second_identity = controller.Find(dispatcher, "pkg-b", "module-b",
                                          "temporary-topic", "temporary-type");
  ASSERT_NE(first_identity, nullptr);
  ASSERT_NE(second_identity, nullptr);
  EXPECT_NE(first_identity->logical, second_identity->logical);
  const auto json = controller.registry().SnapshotJsonl();
  EXPECT_NE(json.find("\"pkg\":\"pkg-a\",\"module\":\"module-a\",\"topic\":\"temporary-topic\"") ,
            std::string::npos);
  EXPECT_NE(json.find("\"pkg\":\"pkg-b\",\"module\":\"module-b\",\"topic\":\"temporary-topic\"") ,
            std::string::npos);
  controller.Start();
  EXPECT_TRUE(controller.Register(dispatcher, nullptr, "pkg", "module",
                                  "later", "type", drivers));
  EXPECT_TRUE(controller.FinalizeSnapshot());
  unsetenv("IBMW_CORE_METRICS");
}

TEST_F(CoreMetrics, UnstartedAndRepeatedFinalizeDoNotAffectStartedInstance) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* started = reinterpret_cast<const void*>(0x2001);
  const void* unstarted = reinterpret_cast<const void*>(0x2002);
  controller.Initialize(started);
  controller.Initialize(unstarted);
  controller.Start(started);

  EXPECT_TRUE(controller.FinalizeSnapshot(unstarted));
  EXPECT_TRUE(controller.FinalizeSnapshot(unstarted));
  EXPECT_TRUE(controller.registry().IsFrozen());
  EXPECT_TRUE(controller.FinalizeSnapshot(started));
  EXPECT_TRUE(controller.FinalizeSnapshot(started));

  unsetenv("IBMW_CORE_METRICS");
}

TEST_F(CoreMetrics, OnlyLastStartedInstanceFinalizesThenReclaimsGeneration) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  const auto dir = std::filesystem::temp_directory_path() / "ibmw_core_metrics_lifecycle_test";
  std::filesystem::remove_all(dir);
  ASSERT_EQ(setenv("IBMW_OBSERVABILITY_OUTPUT_DIR", dir.c_str(), 1), 0);

  auto& controller = Controller::Instance();
  const void* first = reinterpret_cast<const void*>(0x3001);
  const void* second = reinterpret_cast<const void*>(0x3002);
  const void* dispatcher = reinterpret_cast<const void*>(0x3003);
  const auto* wrapper = reinterpret_cast<const transport::SendTypeWrapper*>(0x3004);
  controller.Initialize(first);
  controller.Initialize(second);
  ASSERT_TRUE(controller.Register(dispatcher, wrapper, "pkg", "module",
                                  "topic", "type", {}));
  controller.Start(first);
  controller.Start(second);

  EXPECT_TRUE(controller.FinalizeSnapshot(first));
  EXPECT_FALSE(std::filesystem::exists(dir / "metrics.jsonl"));
  EXPECT_NE(controller.Find(dispatcher, "pkg", "module", "topic", "type"),
            nullptr);

  EXPECT_TRUE(controller.FinalizeSnapshot(second));
  EXPECT_TRUE(std::filesystem::exists(dir / "metrics.jsonl"));
  EXPECT_EQ(controller.Find(dispatcher, "pkg", "module", "topic", "type"),
            nullptr);
  EXPECT_EQ(controller.StorageSizeForTesting(), 0U);

  controller.Initialize(first);
  controller.Start(first);
  EXPECT_TRUE(controller.FinalizeSnapshot(first));

  unsetenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
  unsetenv("IBMW_CORE_METRICS");
  std::filesystem::remove_all(dir);
}

TEST_F(CoreMetrics, StaggeredLifecycleFreezeDoesNotFailRegistration) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* first = reinterpret_cast<const void*>(0x6101);
  const void* second = reinterpret_cast<const void*>(0x6102);
  const void* dispatcher = reinterpret_cast<const void*>(0x6103);
  const auto* wrapper =
      reinterpret_cast<const transport::SendTypeWrapper*>(0x6104);

  controller.Initialize(first);
  controller.Start(first);
  ASSERT_TRUE(controller.registry().IsFrozen());
  controller.Initialize(second);
  EXPECT_TRUE(controller.Register(dispatcher, wrapper, "pkg", "module",
                                  "topic", "type", {}));
  EXPECT_EQ(controller.Find(dispatcher, wrapper), nullptr);

  controller.Start(second);
  EXPECT_TRUE(controller.FinalizeSnapshot(first));
  EXPECT_TRUE(controller.FinalizeSnapshot(second));
}

TEST_F(CoreMetrics, RegistryResetClearsViewsAndOwningStorage) {
  Registry registry;
  ASSERT_NE(registry.RegisterLogicalSlot("old", "type"), nullptr);
  auto receive = registry.RegisterLoanedReceiveSlot("old", "type", "driver");
  ASSERT_NE(receive, nullptr);
  std::weak_ptr<Registry::LoanedReceiveSlot> weak_receive = receive;
  receive.reset();
  ASSERT_EQ(registry.StorageSizeForTesting(), 2U);
  registry.Reset();
  EXPECT_EQ(registry.StorageSizeForTesting(), 0U);
  EXPECT_TRUE(weak_receive.expired());
  EXPECT_TRUE(registry.SnapshotJsonl().empty());
}

TEST_F(CoreMetrics, ResetForTestingDrainsGatesBeforeReclaimingStorage) {
  using namespace std::chrono_literals;
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = Controller::Instance();
  const void* lifecycle = reinterpret_cast<const void*>(0x7101);
  const void* dispatcher = reinterpret_cast<const void*>(0x7102);
  const void* registration = reinterpret_cast<const void*>(0x7103);
  controller.Initialize(lifecycle);
  auto gate = std::make_shared<ConcurrentReceiveGate>();
  ASSERT_NE(controller.RegisterLoanedReceive(dispatcher, registration, "topic",
                                              "Frame", "driver", gate),
            nullptr);

  std::mutex callback_mutex;
  std::condition_variable callback_state;
  bool entered = false;
  bool release_callback = false;
  std::thread callback([&] {
    gate->Invoke(
        [&] {
          std::unique_lock lock(callback_mutex);
          entered = true;
          callback_state.notify_all();
          callback_state.wait(lock, [&] { return release_callback; });
        },
        [] {});
  });
  {
    std::unique_lock lock(callback_mutex);
    callback_state.wait(lock, [&] { return entered; });
  }
  auto reset = std::async(std::launch::async,
                          [&] { controller.ResetForTesting(); });
  gate->WaitForWaiters(1);
  EXPECT_EQ(reset.wait_for(0ms), std::future_status::timeout);
  EXPECT_GT(controller.StorageSizeForTesting(), 0U);
  {
    std::lock_guard lock(callback_mutex);
    release_callback = true;
  }
  callback_state.notify_all();
  callback.join();
  EXPECT_EQ(reset.wait_for(1s), std::future_status::ready);
  reset.get();
  EXPECT_EQ(controller.StorageSizeForTesting(), 0U);
}

TEST_F(CoreMetrics, RepeatedStartedGenerationsKeepStorageBounded) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  const auto dir = std::filesystem::temp_directory_path() /
                   "ibmw_core_metrics_restart_test";
  std::filesystem::remove_all(dir);
  ASSERT_EQ(setenv("IBMW_OBSERVABILITY_OUTPUT_DIR", dir.c_str(), 1), 0);
  auto& controller = Controller::Instance();
  const void* dispatcher = reinterpret_cast<const void*>(0x7200);

  for (uintptr_t generation = 1; generation <= 20; ++generation) {
    const void* lifecycle = reinterpret_cast<const void*>(0x7300 + generation);
    const auto* wrapper = reinterpret_cast<const transport::SendTypeWrapper*>(
        0x7400 + generation);
    controller.Initialize(lifecycle);
    ASSERT_TRUE(controller.Register(dispatcher, wrapper, "pkg", "module",
                                    "topic", "type", {}));
    EXPECT_EQ(controller.StorageSizeForTesting(), 2U);
    controller.Start(lifecycle);
    ASSERT_TRUE(controller.FinalizeSnapshot(lifecycle));
    EXPECT_EQ(controller.StorageSizeForTesting(), 0U);
    EXPECT_EQ(controller.Find(dispatcher, wrapper), nullptr);
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "metrics.jsonl"));
  std::filesystem::remove_all(dir);
}
}  // namespace ibmw::runtime::core::observability
