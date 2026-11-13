// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/engine.h"
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "engine/core_types.h"
#include "engine/yaml_tools.h"
#include "base/sys_tools.h"
#include "base/ibmw_config.h"
#if defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)
#include "ibmw_selfdev/coro/task.h"
#endif

namespace ibmw::runtime::core {
#ifdef IBMW_ENABLE_CORE_METRICS
using observability::CoreMetricsController;
#endif

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Engine::Engine()
    : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {
#if defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)
  // A real reference into libibmw_selfdev_coro, so every executable that
  // embeds the engine (bin/ibmw, ibmw_add_app apps) records it as DT_NEEDED
  // even when linked with --as-needed (the engine itself awaits no Task).
  // The library is then loaded at start-up, before any package: packages
  // that need it resolve to this copy without a RUNPATH of their own, and
  // its initial-exec TLS slot lives in the static TLS block.
  (void)ibmw::selfdev::coro::detail::CurrentStartRecord();
#endif
  BuildSubsystemTable();
  ResolveInitOrder();
}

Engine::~Engine() {
  try {
    // AsyncStart's worker waits on this promise. Signal it before joining;
    // running teardown first would leave the worker blocked forever.
    Shutdown();
  } catch (const std::exception& e) {
    MW_ERROR("Engine destructor could not signal shutdown: {}", e.what());
  } catch (...) {
    MW_ERROR("Engine destructor could not signal shutdown: unknown exception");
  }
  if (async_shutdown_thread_.joinable()) {
    async_shutdown_thread_.join();
  }
  // Nothing to tear down when Initialize never ran or already rolled back.
  const Phase phase = GetPhase();
  if (!shutdown_impl_flag_.load(std::memory_order_acquire) && phase != Phase::kConfigure &&
      phase != Phase::kStopped) {
    MW_WARN("Engine teardown was not completed by the async worker; retrying once after join.");
    if (!ShutdownImpl()) {
      MW_ERROR("Engine destructor shutdown retry was vetoed during transport preparation.");
    }
  }
}

// ---------------------------------------------------------------------------
// BuildSubsystemTable — declarative catalog of all subsystems.
//
// Each entry declares:
//   • id / name / config_key  — identity and YAML key
//   • instance                — polymorphic ISubsystem pointer
//   • depends_on              — subsystems that must initialise first
//   • pre_init / post_init / pre_shutdown — lifecycle hooks
//
// The actual initialization order is derived by topological sort in
// ResolveInitOrder(), NOT by the vector index order.
// ---------------------------------------------------------------------------

void Engine::BuildSubsystemTable() {
  auto get_executor_fn = std::bind(&Engine::GetExecutor, this, std::placeholders::_1);

  subsystems_ = {
      // 0 — Configurator (root; no dependencies, accepts a path or root node)
      {.id = Subsystem::kConfigurator,
       .name = "Configurator",
       .config_key = "",  // Receives the complete configuration.
       .instance = &config_subsystem_,
       .depends_on = {},
       .pre_init = {},   // Engine::Initialize handles the special path-based call.
       .post_init = {},
       .pre_shutdown = {}},

      // 1 — Extension
      {.id = Subsystem::kExtension,
       .name = "Extension",
       .config_key = "extensions",
       .instance = &extension_bus_,
       .depends_on = {Subsystem::kConfigurator},
       .pre_init = [this] { extension_bus_.RegisterCorePtr(this); },
       .post_init = {},
       .pre_shutdown = {}},

      // 2 — Main Thread Executor
      {.id = Subsystem::kMainThread,
       .name = "MainThread",
       .config_key = "main_loop",
       .instance = &main_loop_executor_,
       .depends_on = {Subsystem::kConfigurator},
       .pre_init = {},
       .post_init = {},
       .pre_shutdown = {}},

      // 3 — Watchdog Executor
      {.id = Subsystem::kWatchdog,
       .name = "Watchdog",
       .config_key = "watchdog",
       .instance = &watchdog_executor_,
       .depends_on = {Subsystem::kConfigurator},
       .pre_init = {},
       .post_init = {},
       .pre_shutdown = {}},

      // 4 — Executor Bus
      {.id = Subsystem::kExecutor,
       .name = "Executor",
       .config_key = "scheduling",
       .instance = &executor_bus_,
       .depends_on = {Subsystem::kConfigurator, Subsystem::kMainThread, Subsystem::kWatchdog},
       .pre_init = [this] {
         executor_bus_.SetUsedExecutorName(main_loop_executor_.Name());
         executor_bus_.SetUsedExecutorName(watchdog_executor_.Name());
       },
       .post_init = {},
       .pre_shutdown = {}},

      // 5 — Logger
      {.id = Subsystem::kLogger,
       .name = "Logger",
       .config_key = "logging",
       .instance = &logging_subsystem_,
       .depends_on = {Subsystem::kExecutor},
       .pre_init = [this, get_executor_fn] {
         logging_subsystem_.RegisterGetExecutorFunc(get_executor_fn);
       },
       .post_init = [this] {
         SetCoreLogger();
         PrintStartupInfo();
       },
       .pre_shutdown = [this] {
         ResetCoreLogger();
         logging_subsystem_.Flush();  // waits for the queued entries (CORE-015)
       }},

      // 6 — Allocator
      {.id = Subsystem::kAllocator,
       .name = "Allocator",
       .config_key = "memory",
       .instance = &memory_bus_,
       .depends_on = {Subsystem::kLogger},
       .pre_init = {},
       .post_init = {},
       .pre_shutdown = {}},

      // 7 — Service (RPC)
      {.id = Subsystem::kService,
       .name = "Service",
       .config_key = "service",
       .instance = &service_dispatcher_,
       .depends_on = {Subsystem::kLogger, Subsystem::kExecutor},
       .pre_init = [this, get_executor_fn] {
         service_dispatcher_.RegisterGetExecutorFunc(get_executor_fn);
       },
       .post_init = {},
       .pre_shutdown = {}},

      // 8 — Transport
      {.id = Subsystem::kTransport,
       .name = "Transport",
       .config_key = "messaging",
       .instance = &transport_bus_,
       .depends_on = {Subsystem::kLogger, Subsystem::kExecutor},
       .pre_init = [this, get_executor_fn] {
         transport_bus_.RegisterGetExecutorFunc(get_executor_fn);
#ifdef IBMW_ENABLE_CORE_METRICS
         CoreMetricsController().Initialize(this);
#endif
       },
       .post_init = {},
       .pre_shutdown = {}},

      // 9 — Parameter
      {.id = Subsystem::kParameter,
       .name = "Parameter",
       .config_key = "parameter",
       .instance = &parameter_store_,
       .depends_on = {Subsystem::kLogger},
       .pre_init = {},
       .post_init = {},
       .pre_shutdown = {}},

      // 10 — Module (Container — depends on everything)
      {.id = Subsystem::kModule,
       .name = "Container",
       .config_key = "modules",
       .instance = &module_bus_,
       .depends_on = {Subsystem::kConfigurator, Subsystem::kExtension,
                      Subsystem::kExecutor, Subsystem::kLogger,
                      Subsystem::kAllocator, Subsystem::kService,
                      Subsystem::kTransport, Subsystem::kParameter},
       .pre_init = [this] {
         module_bus_.RegisterCoreAccessConfigurator(
             std::bind(&Engine::InitCoreAccess, this,
                       std::placeholders::_1, std::placeholders::_2));
       },
       .post_init = {},
       .pre_shutdown = {}},
      // 11 — Runtime Graph (advertisements and runtime control endpoint)
      {.id = Subsystem::kRuntimeGraph,
       .name = "RuntimeGraph",
       .config_key = "runtime_graph",
       .instance = &runtime_graph_subsystem_,
       .depends_on = {Subsystem::kModule},
       .pre_init = [this] { runtime_graph_subsystem_.SetEngine(this); },
       .post_init = {},
       .pre_shutdown = {}},
  };
}

// ---------------------------------------------------------------------------
// ResolveInitOrder — topological sort (Kahn's algorithm).
//
// Builds an index mapping from Subsystem enum → vector index, then computes
// in-degrees and iterates using a queue to produce a valid init order.
// Throws if a cycle is detected (should never happen with correct table).
// ---------------------------------------------------------------------------

void Engine::ResolveInitOrder() {
  const size_t n = subsystems_.size();

  // Map Subsystem id → index in subsystems_ vector.
  std::unordered_map<uint8_t, size_t> id_to_idx;
  id_to_idx.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    id_to_idx[static_cast<uint8_t>(subsystems_[i].id)] = i;
  }

  // Compute in-degrees.
  std::vector<size_t> in_degree(n, 0);
  for (size_t i = 0; i < n; ++i) {
    in_degree[i] = subsystems_[i].depends_on.size();
  }

  // Seed the queue with zero-degree nodes.
  std::queue<size_t> ready;
  for (size_t i = 0; i < n; ++i) {
    if (in_degree[i] == 0) ready.push(i);
  }

  // Build adjacency list (dependency → dependents).
  // adj[i] = list of indices that depend on subsystems_[i].
  std::vector<std::vector<size_t>> adj(n);
  for (size_t i = 0; i < n; ++i) {
    for (auto dep : subsystems_[i].depends_on) {
      auto it = id_to_idx.find(static_cast<uint8_t>(dep));
      MW_REQUIRE(it != id_to_idx.end(),
                 "Subsystem '{}' declares dependency on unknown subsystem id {}.",
                 subsystems_[i].name, static_cast<int>(dep));
      adj[it->second].push_back(i);
    }
  }

  init_order_.clear();
  init_order_.reserve(n);

  while (!ready.empty()) {
    size_t idx = ready.front();
    ready.pop();
    init_order_.push_back(idx);

    for (size_t dependent : adj[idx]) {
      if (--in_degree[dependent] == 0) {
        ready.push(dependent);
      }
    }
  }

  MW_REQUIRE(init_order_.size() == n,
             "Dependency cycle detected among subsystems! Only {} of {} resolved.",
             init_order_.size(), n);

  // Shutdown order: reverse topological order, except that extensions must
  // initialize early (they register drivers via hooks) yet stop before the
  // logger/executors they hold handles to. Move Extension right before Logger,
  // which is still after everything that depends on it (e.g. Container).
  shutdown_order_.assign(init_order_.rbegin(), init_order_.rend());
  const auto ext_it = id_to_idx.find(static_cast<uint8_t>(Subsystem::kExtension));
  const auto log_it = id_to_idx.find(static_cast<uint8_t>(Subsystem::kLogger));
  if (ext_it != id_to_idx.end() && log_it != id_to_idx.end()) {
    auto ext_pos = std::find(shutdown_order_.begin(), shutdown_order_.end(), ext_it->second);
    auto log_pos = std::find(shutdown_order_.begin(), shutdown_order_.end(), log_it->second);
    if (log_pos < ext_pos) std::rotate(log_pos, ext_pos, ext_pos + 1);
  }

  // Every subsystem must stop before the subsystems it depends on.
  std::vector<size_t> stop_rank(n);
  for (size_t r = 0; r < n; ++r) stop_rank[shutdown_order_[r]] = r;
  for (size_t i = 0; i < n; ++i) {
    for (auto dep : subsystems_[i].depends_on) {
      MW_REQUIRE(stop_rank[i] < stop_rank[id_to_idx[static_cast<uint8_t>(dep)]],
                 "Subsystem '{}' would stop after its dependency.", subsystems_[i].name);
    }
  }
}

// ---------------------------------------------------------------------------
// RunHooks — fire all callbacks registered at the given hook point.
// ---------------------------------------------------------------------------

std::vector<Engine::HookTask> Engine::SnapshotHooks(HookPoint point) const {
  std::lock_guard<std::mutex> lck(hooks_mutex_);
  auto it = hooks_.find(point);
  if (it == hooks_.end()) return {};
  return it->second;
}

void Engine::RunHooks(HookPoint point) {
  for (const auto& fn : SnapshotHooks(point)) fn();
}

// ---------------------------------------------------------------------------
// Initialize — iterate the subsystem table in topological order.
//
// For each subsystem:
//   1. Fire Before(kInitialize, subsystem) hooks
//   2. Run pre_init() hook (dependency injection)
//   3. Inject shared logger via SetLogger()
//   4. Call Initialize() with the appropriate YAML config node
//      (ConfigSubsystem is special: uses path-based init)
//   5. Run post_init() hook
//   6. Fire After(kInitialize, subsystem) hooks
//
// On failure, rolls back the failed and all previously-initialized subsystems
// in shutdown order.
// ---------------------------------------------------------------------------

void Engine::Initialize(const Options& options) {
  MW_REQUIRE(options.cfg_file_path.empty() || !options.config_node.IsDefined(),
             "Specify either cfg_file_path or config_node, not both.");
  phase_.store(Phase::kConfigure, std::memory_order_release);
  options_ = options;

  phase_.store(Phase::kInitialize, std::memory_order_release);

  size_t initialized_count = 0;
  try {
    for (size_t pos : init_order_) {
      auto& entry = subsystems_[pos];

      RunHooks(Before(Phase::kInitialize, entry.id));

      // Pre-init dependency injection.
      if (entry.pre_init) entry.pre_init();

      // Inject shared logger.
      entry.instance->SetLogger(logger_ptr_);

      // Both configuration sources share normalization and subsystem initialization.
      if (entry.id == Subsystem::kConfigurator) {
        if (options_.config_node.IsDefined()) {
          config_subsystem_.Initialize(options_.config_node);
        } else {
          config_subsystem_.Initialize(options_.cfg_file_path);
        }
        runtime_graph_subsystem_.Configure(
            config_subsystem_.GetIbmwOptionsNode("runtime_graph"));
      } else {
        entry.instance->Initialize(
            config_subsystem_.GetIbmwOptionsNode(entry.config_key));
      }

      // Post-init actions (e.g. SetCoreLogger, PrintStartupInfo).
      if (entry.post_init) entry.post_init();

      RunHooks(After(Phase::kInitialize, entry.id));
      ++initialized_count;
    }

    // Validate the config file after all subsystems have registered their keys.
    CheckCfgFile();

    // After all subsystems are initialized (so transport_bus knows its active
    // drivers AND module_bus has loaded all packages and recorded all
    // directly-registered modules), broadcast every used transport driver
    // pointer into every module known to the bus via typed-slot dispatch.
    // Modules opt in via the IBMW_USE_DRIVER / IBMW_USE_DRIVERS macros;
    // modules that don't opt in see no effect (the base-class
    // SetDriverSlot no-op runs).
    //
    // Targets BOTH pkg-loaded modules AND standalone-mode modules
    // registered via ModuleBus::RegisterModule, so standalone main.cc is
    // identical to pkg mode from the developer's perspective.
    for (auto* td : transport_bus_.GetUsedTransportDriver()) {
      module_bus_.BroadcastDriverSlot(td->Slot(), static_cast<void*>(td));
    }
  } catch (...) {
    // Rollback in shutdown order, including the subsystem that failed: it may
    // already own threads or resources. Subsystem Shutdown() is idempotent and
    // state-guarded, so it is safe on a partially initialized instance.
    const size_t rollback_count = std::min(initialized_count + 1, init_order_.size());
    if (initialized_count < init_order_.size()) {
      MW_WARN("Initialization failed at subsystem '{}'; rolling back {} subsystem(s).",
              subsystems_[init_order_[initialized_count]].name, rollback_count);
    } else {
      MW_WARN("Post-initialization step failed; rolling back {} subsystem(s).", rollback_count);
    }
    std::vector<bool> to_rollback(subsystems_.size(), false);
    for (size_t i = 0; i < rollback_count; ++i) to_rollback[init_order_[i]] = true;
    for (size_t pos : shutdown_order_) {
      if (!to_rollback[pos]) continue;
      auto& rb_entry = subsystems_[pos];
      try {
        if (rb_entry.pre_shutdown) rb_entry.pre_shutdown();
        rb_entry.instance->Shutdown();
      } catch (const std::exception& e) {
        MW_ERROR("Rollback shutdown of '{}' failed: {}", rb_entry.name, e.what());
      } catch (...) {
        MW_ERROR("Rollback shutdown of '{}' failed: unknown exception", rb_entry.name);
      }
    }
    phase_.store(Phase::kStopped, std::memory_order_release);
    throw;
  }
}

// ---------------------------------------------------------------------------
// StartImpl — iterate the subsystem table in topological order.
// ---------------------------------------------------------------------------

void Engine::StartImpl() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  phase_.store(Phase::kStart, std::memory_order_release);
#ifdef IBMW_ENABLE_CORE_METRICS
  CoreMetricsController().Start(this);
#endif

  MW_INFO("Gen initialization report:\n{}", GenInitializationReport());

  // Initialize() dlopen'ed more libraries after ModuleBus refreshed the
  // crash handler's module table (TypeSupport packages loaded by
  // RuntimeGraph, plugins loaded by extensions): record them before any
  // Start() runs their code. Libraries loaded later on demand (for example a
  // discovery plugin created by a topic subscription) show as bare pcs in the
  // raw part of a crash report; the symbolized part still names them.
  logger::CrashSignalHandling::RefreshModules();

  RunHooks(BeforePhase(Phase::kStart));

  for (size_t pos : init_order_) {
    auto& entry = subsystems_[pos];
    RunHooks(Before(Phase::kStart, entry.id));
    if (entry.id == Subsystem::kService) {
      runtime_graph_subsystem_.RefreshServiceList();
    }
    if (entry.id == Subsystem::kModule) {
      runtime_graph_subsystem_.RefreshModuleList();
    }
    entry.instance->Start();
    RunHooks(After(Phase::kStart, entry.id));
  }

  RunHooks(AfterPhase(Phase::kStart));

  phase_.store(Phase::kRunning, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ShutdownImpl — iterate the subsystem table in shutdown_order_ (reverse topological, Extension before Logger).
// ---------------------------------------------------------------------------

bool Engine::ShutdownImpl() {
  bool expected = false;
  if (!shutdown_impl_in_progress_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return true;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  // kConfigure: Initialize never ran, no subsystem holds anything.
  // kStopped: torn down already, by an earlier call or the Initialize
  // rollback; running every Shutdown() again would include subsystems that
  // were never initialized. The flag stays unset so a later Initialize can
  // still be shut down.
  const Phase phase = phase_.load(std::memory_order_acquire);
  if (shutdown_impl_flag_.load(std::memory_order_acquire) || phase == Phase::kConfigure ||
      phase == Phase::kStopped) {
    shutdown_impl_in_progress_.store(false, std::memory_order_release);
    return true;
  }
  if (!transport_bus_.TryPrepareShutdown()) {
    MW_WARN("Engine shutdown was vetoed during transport preparation; no "
            "subsystem teardown was started and shutdown remains retryable.");
    shutdown_impl_in_progress_.store(false, std::memory_order_release);
    return false;
  }

  MW_INFO("Shutting down...");

  phase_.store(Phase::kStopping, std::memory_order_release);

  std::string first_error;
  size_t error_count = 0;
  const auto record_error = [this, &first_error, &error_count](
                                std::string_view operation,
                                std::string_view detail) {
    ++error_count;
    if (first_error.empty()) {
      first_error = std::string(operation) + ": " + std::string(detail);
    }
    MW_ERROR("Engine teardown '{}' failed: {}", operation, detail);
  };
  const auto run_stage = [&record_error](std::string_view operation,
                                         const auto& fn) {
    try {
      fn();
    } catch (const std::exception& e) {
      record_error(operation, e.what());
    } catch (...) {
      record_error(operation, "unknown exception");
    }
  };
  const auto run_shutdown_hooks = [this, &run_stage](HookPoint point,
                                                     std::string_view name) {
    const auto hooks = SnapshotHooks(point);
    for (size_t i = 0; i < hooks.size(); ++i) {
      run_stage(std::string(name) + " hook #" + std::to_string(i), hooks[i]);
    }
  };

  run_shutdown_hooks(BeforePhase(Phase::kStopping), "before stopping phase");

  // Dependency-respecting shutdown order (see ResolveInitOrder()).
  for (size_t pos : shutdown_order_) {
    auto& entry = subsystems_[pos];
    run_shutdown_hooks(Before(Phase::kStopping, entry.id),
                       std::string("before ") + std::string(entry.name));
    if (entry.pre_shutdown) {
      run_stage(std::string(entry.name) + " pre_shutdown", entry.pre_shutdown);
    }
    run_stage(std::string(entry.name) + " Shutdown",
              [&entry] { entry.instance->Shutdown(); });
    run_shutdown_hooks(After(Phase::kStopping, entry.id),
                       std::string("after ") + std::string(entry.name));
  }

#ifdef IBMW_ENABLE_CORE_METRICS
  if (!CoreMetricsController().FinalizeSnapshot(this))
    MW_ERROR("Core Metrics V1 failed to write metrics.jsonl");
#endif

  run_shutdown_hooks(AfterPhase(Phase::kStopping), "after stopping phase");

  phase_.store(Phase::kStopped, std::memory_order_release);
  shutdown_impl_flag_.store(true, std::memory_order_release);
  shutdown_impl_in_progress_.store(false, std::memory_order_release);
  if (error_count != 0) {
    MW_ERROR("Engine teardown completed with {} error(s); first error: {}",
             error_count, first_error);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Start / AsyncStart / Shutdown — public entry points.
// ---------------------------------------------------------------------------

void Engine::Start() {
  StartImpl();

  MW_INFO("Running. Waiting for shutdown signal.");

  shutdown_promise_.get_future().wait();
  ShutdownImpl();
}

std::future<void> Engine::AsyncStart() {
  StartImpl();

  MW_INFO("Running (async). Waiting for shutdown signal.");

  std::packaged_task<void()> task([this]() {
    shutdown_promise_.get_future().wait();
    ShutdownImpl();
  });

  auto fu = task.get_future();

  async_shutdown_thread_ = std::thread(std::move(task));

  return fu;
}

void Engine::Shutdown() {
  if (!std::atomic_exchange(&shutdown_flag_, true)) {
    MW_INFO("Shutdown signal received.");
    shutdown_promise_.set_value();
    return;
  }

  if (shutdown_impl_in_progress_.load(std::memory_order_acquire) ||
      phase_.load(std::memory_order_acquire) == Phase::kStopping) {
    return;
  }

  // The first call remains signal-only. If the worker's prepare was vetoed,
  // a later public call performs a real retry instead of being discarded.
  if (!shutdown_impl_flag_.load(std::memory_order_acquire) ||
      phase_.load(std::memory_order_acquire) != Phase::kStopped) {
    ShutdownImpl();
  }
}

// ---------------------------------------------------------------------------
// PrintStartupInfo — IBMW banner.
// ---------------------------------------------------------------------------

void Engine::PrintStartupInfo() const {
  constexpr const auto* version_str = util::GetIbmwVersion();
  MW_INFO("IBMW v{} | IB Middleware Framework", version_str);
}

// ---------------------------------------------------------------------------
// Logger helpers
// ---------------------------------------------------------------------------

void Engine::SetCoreLogger() {
  const auto* core_logger_ptr = logging_subsystem_.GetLoggerAccess("core").NativeHandle();

  logger_ptr_->get_log_level_func = [core_logger_ptr]() -> uint32_t {
    return core_logger_ptr->get_log_level(core_logger_ptr->ctx);
  };

  logger_ptr_->log_func =
      [core_logger_ptr](
          uint32_t lvl,
          uint32_t line,
          const char* file_name,
          const char* function_name,
          const char* log_data,
          size_t log_data_size) {
        core_logger_ptr->log(
            core_logger_ptr->ctx,
            static_cast<ibmw_log_level_t>(lvl),
            line, file_name, function_name,
            log_data, log_data_size);
      };
}

void Engine::ResetCoreLogger() {
  logger_ptr_->get_log_level_func = ibmw::common::util::SimpleLogger::GetLogLevel;
  logger_ptr_->log_func = ibmw::common::util::SimpleLogger::Log;
}

// ---------------------------------------------------------------------------
// Executor lookup
// ---------------------------------------------------------------------------

ibmw::executor::ExecutorHandle Engine::GetExecutor(std::string_view executor_name) {
  if (executor_name.empty() || executor_name == watchdog_executor_.Name())
    return ibmw::executor::ExecutorHandle(watchdog_executor_.NativeHandle());
  return GetExecutorBus().GetExecutor(executor_name);
}

// ---------------------------------------------------------------------------
// Module access configuration
// ---------------------------------------------------------------------------

void Engine::InitCoreAccess(const util::ModuleDetailInfo& info, module::CoreAccess& access) {
  access.SetConfigurator(config_subsystem_.GetConfiguratorAccess(info).NativeHandle());
  access.SetExecutorBus(executor_bus_.GetExecutorAccess(info).NativeHandle());
  access.SetLogger(logging_subsystem_.GetLoggerAccess(info).NativeHandle());
  access.SetMemory(memory_bus_.GetAllocatorAccess(info).NativeHandle());
  access.SetServiceHandle(service_dispatcher_.GetRpcAccess(info).NativeHandle());
  access.SetTransportHandle(transport_bus_.GetBusAccess(info).NativeHandle());
  access.SetParameterHandle(parameter_store_.GetParamsAccess(info).NativeHandle());
}

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

void Engine::CheckCfgFile() const {
  std::string check_msg = util::CheckYamlNodes(
      config_subsystem_.GetRootOptionsNode()["ibmw"],
      config_subsystem_.GetUserRootOptionsNode()["ibmw"],
      "ibmw");

  if (!check_msg.empty())
    MW_WARN("Configuration Name Warning in \"{}\":\n{}",
               config_subsystem_.GetConfigureFilePath(), check_msg);
}

// ---------------------------------------------------------------------------
// Initialization report (R15 — unified polymorphic iteration)
//
// Iterates subsystems in init_order_ and calls GenInitializationReport()
// polymorphically via the ISubsystem interface.  Only non-empty reports
// are included.
// ---------------------------------------------------------------------------

std::string Engine::GenInitializationReport() const {
  // main.cc dumps the report between Initialize() and Start(), where the
  // phase is still kInitialize.
  const Phase phase = phase_.load(std::memory_order_acquire);
  MW_REQUIRE(phase == Phase::kInitialize || phase == Phase::kStart || phase == Phase::kRunning,
             "Initialization report can only be generated after initialization.");

  std::list<std::pair<std::string, std::string>> report;

  // System info header — always first.
  std::vector<std::vector<std::string>> base_info_table = {
      {"Version", util::GetIbmwVersion()},
      {"Config", config_subsystem_.GetConfigureFilePath()},
      {"Executable", ibmw::common::util::GetExecutablePath()},
      {"PID", ibmw::common::util::GetCurrentProcessPid()},
  };
  report.emplace_back("System", ibmw::common::util::FormatTree(base_info_table, false));

  // Polymorphic report generation — iterate in init order.
  for (size_t pos : init_order_) {
    const auto& entry = subsystems_[pos];
    auto sub_report = entry.instance->GenInitializationReport();
    if (!sub_report.empty()) {
      report.splice(report.end(), std::move(sub_report));
    }
  }

  std::stringstream result;
  result << "\n";

  for (auto& itr : report) {
    result << "--- " << itr.first << " ---\n" << itr.second << "\n";
  }

  return result.str();
}

}  // namespace ibmw::runtime::core
