// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Engine — central runtime orchestrator.
//
// Lifecycle phases:  Configure → Resolve → Initialize → Start → Running → Stopping → Stopped
// Subsystems are registered in a declarative table with explicit dependency edges;
// a topological sort determines initialization order automatically.
// Extensions hook into (Phase × Subsystem × Timing) points.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/isubsystem.h"
#include "engine/module/memory_bus.h"
#include "engine/transport/transport_bus.h"
#include "engine/module/config_subsystem.h"
#include "engine/executor/executor_bus.h"
#include "engine/executor/watchdog_executor.h"
#include "engine/executor/main_loop_executor.h"
#include "engine/logger/logging_subsystem.h"
#include "engine/module/module_bus.h"
#include "engine/module/parameter_store.h"
#include "engine/extension/extension_bus.h"
#include "engine/service/service_dispatcher.h"
#include "engine/runtime_graph/runtime_graph_subsystem.h"
#include "base/log_util.h"

namespace ibmw::runtime::core {

class EngineShutdownFixture;

// ---------------------------------------------------------------------------
// Phase — the 7 lifecycle phases of the IBMW runtime.
// ---------------------------------------------------------------------------
enum class Phase : uint8_t {
  kConfigure,   // Load and parse configuration files.
  kResolve,     // Wire subsystem dependencies (plugins, executors, etc.).
  kInitialize,  // Initialize all subsystems.
  kStart,       // Start all subsystems.
  kRunning,     // Steady state — modules are executing.
  kStopping,    // Shut down subsystems (reverse order).
  kStopped,     // All subsystems have been torn down.
};

// ---------------------------------------------------------------------------
// Subsystem — identifies each registered bus/service.
// ---------------------------------------------------------------------------
enum class Subsystem : uint8_t {
  kConfigurator,
  kExtension,
  kMainThread,
  kWatchdog,
  kExecutor,
  kLogger,
  kAllocator,
  kService,
  kTransport,
  kParameter,
  kModule,
  kRuntimeGraph,

  kCount,  // sentinel — total number of subsystems
};

// ---------------------------------------------------------------------------
// HookTiming — before or after a subsystem transition.
// ---------------------------------------------------------------------------
enum class HookTiming : uint8_t {
  kBefore,
  kAfter,
};

// ---------------------------------------------------------------------------
// HookPoint — a (Phase, Subsystem, Timing) triple that uniquely identifies
// a point in the lifecycle where extension code may execute.
//
// Special points use Subsystem::kCount as a wildcard:
//   {kInitialize, kCount, kBefore}  →  "before any subsystem initializes"
//   {kStopping,   kCount, kBefore}  →  "before shutdown begins"
//   {kStopping,   kCount, kAfter}   →  "after all subsystems shut down"
//   {kStart,      kCount, kBefore}  →  "before any subsystem starts"
//   {kStart,      kCount, kAfter}   →  "after all subsystems started"
// ---------------------------------------------------------------------------
struct HookPoint {
  Phase phase;
  Subsystem subsystem;
  HookTiming timing;

  constexpr bool operator<(const HookPoint& o) const {
    if (phase != o.phase) return phase < o.phase;
    if (subsystem != o.subsystem) return subsystem < o.subsystem;
    return timing < o.timing;
  }

  constexpr bool operator==(const HookPoint& o) const = default;
};

// Convenience factory functions for common hook points.
constexpr HookPoint Before(Phase p, Subsystem s) { return {p, s, HookTiming::kBefore}; }
constexpr HookPoint After(Phase p, Subsystem s)  { return {p, s, HookTiming::kAfter}; }
constexpr HookPoint BeforePhase(Phase p)          { return {p, Subsystem::kCount, HookTiming::kBefore}; }
constexpr HookPoint AfterPhase(Phase p)           { return {p, Subsystem::kCount, HookTiming::kAfter}; }

// ---------------------------------------------------------------------------
// Engine — the runtime orchestrator.
// ---------------------------------------------------------------------------
class Engine {
  friend class EngineShutdownFixture;

 public:
  struct Options {
    std::string cfg_file_path;
    // Optional complete configuration supplied by an embedding application or CLI.
    // Mutually exclusive with cfg_file_path; no configuration file is required.
    YAML::Node config_node{YAML::NodeType::Undefined};
  };

  using HookTask = std::function<void()>;

 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void Initialize(const Options& options);

  void Start();
  std::future<void> AsyncStart();

  void Shutdown();

  Phase GetPhase() const { return phase_.load(std::memory_order_acquire); }

  // Register a hook at an arbitrary lifecycle point.
  template <typename... Args>
    requires std::constructible_from<HookTask, Args...>
  void RegisterHook(HookPoint point, Args&&... args) {
    // Extensions may register from any thread, including from inside a hook.
    std::lock_guard<std::mutex> lck(hooks_mutex_);
    hooks_[point].emplace_back(std::forward<Args>(args)...);
  }

  // Bus accessors — plugins and modules use these.
  auto& GetConfigSubsystem() { return config_subsystem_; }
  auto& GetExtensionBus() { return extension_bus_; }
  auto& GetMainLoopExecutor() { return main_loop_executor_; }
  auto& GetWatchdogExecutor() { return watchdog_executor_; }
  auto& GetExecutorBus() { return executor_bus_; }
  auto& GetLoggingSubsystem() { return logging_subsystem_; }
  auto& GetMemoryBus() { return memory_bus_; }
  auto& GetServiceDispatcher() { return service_dispatcher_; }
  auto& GetTransportBus() { return transport_bus_; }
  auto& GetParameterStore() { return parameter_store_; }
  auto& GetModuleBus() { return module_bus_; }
  auto& GetRuntimeGraphSubsystem() { return runtime_graph_subsystem_; }

  const auto& GetLogger() const { return *logger_ptr_; }

  std::string GenInitializationReport() const;

 private:
  // Subsystem descriptor — one per registered bus.
  // Dependencies are declared explicitly; topological sort derives the order.
  struct SubsystemEntry {
    Subsystem id;
    std::string_view name;
    std::string_view config_key;  // YAML key under "ibmw:" (empty → special init)
    ISubsystem* instance;         // Polymorphic pointer to the concrete subsystem.
    std::vector<Subsystem> depends_on;    // Subsystems that must init before this one.
    std::function<void()> pre_init;       // Dependency injection before Initialize().
    std::function<void()> post_init;      // Actions after Initialize() (e.g. SetCoreLogger).
    std::function<void()> pre_shutdown;   // Actions before Shutdown() (e.g. ResetCoreLogger).
  };

  void BuildSubsystemTable();
  void ResolveInitOrder();
  void RunHooks(HookPoint point);
  void SetCoreLogger();
  void ResetCoreLogger();
  ibmw::executor::ExecutorHandle GetExecutor(std::string_view executor_name);
  void InitCoreAccess(const util::ModuleDetailInfo& info, module::CoreAccess& access);
  void CheckCfgFile() const;
  void StartImpl();
  bool ShutdownImpl();
  void PrintStartupInfo() const;

 private:
  Options options_;
  std::atomic<Phase> phase_{Phase::kConfigure};
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  std::atomic_bool shutdown_flag_ = false;
  std::promise<void> shutdown_promise_;
  std::atomic_bool shutdown_impl_in_progress_ = false;
  std::atomic_bool shutdown_impl_flag_ = false;
  std::mutex lifecycle_mutex_;
  std::thread async_shutdown_thread_;  // Stored to avoid dangling detached thread

  // Hook catalog — ordered map so hooks fire in deterministic order.
  // Lifecycle-only: callers iterate a snapshot (under hooks_mutex_) so hooks
  // run without the lock.
  std::vector<HookTask> SnapshotHooks(HookPoint point) const;
  std::map<HookPoint, std::vector<HookTask>> hooks_;

  // Subsystem table — populated in BuildSubsystemTable().
  std::vector<SubsystemEntry> subsystems_;

  // Initialization order (indices into subsystems_), derived by ResolveInitOrder().
  std::vector<size_t> init_order_;

  // Bus instances.
  configurator::ConfigSubsystem config_subsystem_;
  // Shared TypeSupport libraries outlive all modules and extensions.
  runtime_graph::RuntimeGraphSubsystem runtime_graph_subsystem_;
  extension::ExtensionBus extension_bus_;
  executor::MainLoopExecutor main_loop_executor_;
  executor::WatchdogExecutor watchdog_executor_;
  executor::ExecutorBus executor_bus_;
  logger::LoggingSubsystem logging_subsystem_;
  allocator::MemoryBus memory_bus_;
  service::ServiceDispatcher service_dispatcher_;
  transport::TransportBus transport_bus_;
  parameter::ParameterStore parameter_store_;
  module::ModuleBus module_bus_;

  // Appended after the pre-existing members on purpose: engine.h is installed
  // and has inline accessors, so existing member offsets must not move.
  mutable std::mutex hooks_mutex_;
  // Stop order (indices into subsystems_), derived by ResolveInitOrder().
  std::vector<size_t> shutdown_order_;
};

}  // namespace ibmw::runtime::core
