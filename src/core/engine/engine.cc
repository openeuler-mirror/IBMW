// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/engine.h"

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "engine/core_types.h"
#include "engine/yaml_tools.h"
#include "base/sys_tools.h"

namespace ibmw::runtime::core {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

Engine::Engine()
    : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {
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
  if (!shutdown_impl_flag_.load(std::memory_order_acquire)) {
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
      // 0 — Configurator (root; no dependencies, special path-based init)
      {.id = Subsystem::kConfigurator,
       .name = "Configurator",
       .config_key = "",  // No YAML key — uses file path directly.
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
         logging_subsystem_.Flush();
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
}

// ---------------------------------------------------------------------------
// RunHooks — fire all callbacks registered at the given hook point.
// ---------------------------------------------------------------------------

void Engine::RunHooks(HookPoint point) {
  auto it = hooks_.find(point);
  if (it == hooks_.end()) return;
  for (const auto& fn : it->second) fn();
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
// On failure, rolls back all previously-initialized subsystems in reverse.
// ---------------------------------------------------------------------------

void Engine::Initialize(const Options& options) {
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

      // Initialize: ConfigSubsystem uses path-based init; all others use YAML.
      if (entry.id == Subsystem::kConfigurator) {
        config_subsystem_.Initialize(options_.cfg_file_path);
      } else {
        entry.instance->Initialize(
            config_subsystem_.GetIbmwOptionsNode(entry.config_key));
      }

      // Post-init actions (e.g. SetCoreLogger, PrintStartupInfo).
      if (entry.post_init) entry.post_init();

      RunHooks(After(Phase::kInitialize, entry.id));
      ++initialized_count;
    }
  } catch (...) {
    // Rollback: shut down all previously-initialized subsystems in reverse order.
    size_t failed_pos = init_order_[initialized_count];
    MW_WARN("Initialization failed at subsystem '{}'; rolling back {} initialized subsystem(s).",
            subsystems_[failed_pos].name, initialized_count);
    for (size_t i = initialized_count; i > 0; --i) {
      size_t rollback_pos = init_order_[i - 1];
      auto& rb_entry = subsystems_[rollback_pos];
      try {
        if (rb_entry.pre_shutdown) rb_entry.pre_shutdown();
        rb_entry.instance->Shutdown();
      } catch (const std::exception& e) {
        MW_ERROR("Rollback shutdown of '{}' failed: {}", rb_entry.name, e.what());
      }
    }
    phase_.store(Phase::kStopped, std::memory_order_release);
    throw;
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
}

// ---------------------------------------------------------------------------
// StartImpl — iterate the subsystem table in topological order.
// ---------------------------------------------------------------------------

void Engine::StartImpl() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  phase_.store(Phase::kStart, std::memory_order_release);

  MW_INFO("Gen initialization report:\n{}", GenInitializationReport());

  RunHooks(BeforePhase(Phase::kStart));

  for (size_t pos : init_order_) {
    auto& entry = subsystems_[pos];
    RunHooks(Before(Phase::kStart, entry.id));
    entry.instance->Start();
    RunHooks(After(Phase::kStart, entry.id));
  }

  RunHooks(AfterPhase(Phase::kStart));

  phase_.store(Phase::kRunning, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ShutdownImpl — iterate the subsystem table in REVERSE topological order.
// ---------------------------------------------------------------------------

bool Engine::ShutdownImpl() {
  bool expected = false;
  if (!shutdown_impl_in_progress_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return true;
  }

  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (shutdown_impl_flag_.load(std::memory_order_acquire)) {
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
    const auto hooks = hooks_.find(point);
    if (hooks == hooks_.end()) return;
    for (size_t i = 0; i < hooks->second.size(); ++i) {
      run_stage(std::string(name) + " hook #" + std::to_string(i),
                hooks->second[i]);
    }
  };

  run_shutdown_hooks(BeforePhase(Phase::kStopping), "before stopping phase");

  // Reverse iteration through init_order_ for proper dependency-respecting shutdown.
  for (auto it = init_order_.rbegin(); it != init_order_.rend(); ++it) {
    auto& entry = subsystems_[*it];
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
  MW_REQUIRE(phase_.load(std::memory_order_acquire) == Phase::kStart ||
                 phase_.load(std::memory_order_acquire) == Phase::kRunning,
             "Initialization report can only be generated after initialization is complete.");

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
