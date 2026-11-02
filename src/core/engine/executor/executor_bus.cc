// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/executor_bus.h"
#include "cpp/util/string.h"
#include "engine/executor/strand_executor.h"
#include "engine/executor/thread_pool_executor.h"
#include "engine/executor/dedicated_executor.h"
#include "engine/executor/parallel_executor.h"
#include "engine/executor/timer_executor.h"
#include "base/string_util.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::ExecutorBus::Options> {
  using Options = ibmw::runtime::core::executor::ExecutorBus::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["executors"] = YAML::Node();
    for (const auto& executor : rhs.executors_options) {
      Node executor_node;
      executor_node["name"] = executor.name;
      executor_node["type"] = executor.type;
      executor_node["options"] = executor.options;
      node["executors"].push_back(executor_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["executors"] && node["executors"].IsSequence()) {
      for (const auto& executor_node : node["executors"]) {
        auto executor_options = Options::ExecutorOptions{
            .name = executor_node["name"].as<std::string>(),
            .type = executor_node["type"].as<std::string>()};

        if (executor_node["options"])
          executor_options.options = executor_node["options"];
        else
          executor_options.options = YAML::Node(YAML::NodeType::Null);

        rhs.executors_options.emplace_back(std::move(executor_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::executor {

void ExecutorBus::Initialize(YAML::Node options_node) {
  RegisterAsioExecutorGenFunc();
  RegisterTBBExecutorGenFunc();
  RegisterSimpleThreadExecutorGenFunc();
  RegisterTimeWheelExecutorGenFunc();

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Executor bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  // Generate a executor
  for (auto& executor_options : options_.executors_options) {
    MW_REQUIRE(
        (executor_access_map_.find(executor_options.name) == executor_access_map_.end()) &&
            (used_executor_names_.find(executor_options.name) == used_executor_names_.end()),
        "Duplicate executor name '{}'.", executor_options.name);

    auto finditr = executor_gen_func_map_.find(executor_options.type);
    MW_REQUIRE(finditr != executor_gen_func_map_.end(),
                            "Invalid executor type '{}'.",
                            executor_options.type);

    auto executor_ptr = finditr->second();
    MW_REQUIRE(
        executor_ptr,
        "Gen executor failed, executor name '{}', executor type '{}'.",
        executor_options.name, executor_options.type);

    executor_ptr->Initialize(executor_options.name, executor_options.options);

    MW_TRACE("Gen executor '{}' success.", executor_ptr->Name());

    auto access_ptr = std::make_unique<ExecutorAccess>(executor_ptr.get());

    {
      std::unique_lock<std::shared_mutex> lck(executor_access_mutex_);
      executor_access_map_.emplace(executor_options.name, std::move(access_ptr));
    }

    executor_vec_.emplace_back(std::move(executor_ptr));
  }

  options_node = options_;

  MW_INFO("[Executors] initialized");
}

void ExecutorBus::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (auto& itr : executor_vec_) {
    itr->Start();
  }

  MW_INFO("[Executors] started");
}

void ExecutorBus::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Executors] stopped");

  // Shut down executors FIRST, before destroying access wrappers.
  // Executors may still have in-flight tasks that reference access wrappers.
  // Reverse creation order: an executor bound to another one (strand on a
  // thread pool, timer on a bound executor) is created after it, so it must
  // stop and be destroyed first. A strand outliving its io_context is a UAF.
  // Consequence: a task still draining in an earlier executor that dispatches
  // into a later one is rejected (logged), since that one is already stopped.
  for (auto itr = executor_vec_.rbegin(); itr != executor_vec_.rend(); ++itr) {
    (*itr)->Shutdown();
  }

  executor_bus_access_map_.clear();
  {
    std::unique_lock<std::shared_mutex> lck(executor_access_mutex_);
    executor_access_map_.clear();
  }
  while (!executor_vec_.empty()) executor_vec_.pop_back();
  executor_gen_func_map_.clear();
}

void ExecutorBus::RegisterExecutorGenFunc(
    std::string_view type, ExecutorGenFunc&& executor_gen_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  executor_gen_func_map_.emplace(type, std::move(executor_gen_func));
}

const ExecutorBusAccess& ExecutorBus::GetExecutorAccess(
    const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto itr = executor_bus_access_map_.find(module_info.module_name);
  if (itr != executor_bus_access_map_.end()) return *(itr->second);

  auto access_ptr = std::make_unique<ExecutorBusAccess>(executor_access_map_);
  access_ptr->SetLogger(logger_ptr_);

  auto emplace_ret = executor_bus_access_map_.emplace(module_info.module_name, std::move(access_ptr));

  return *(emplace_ret.first->second);
}

ibmw::executor::ExecutorHandle ExecutorBus::GetExecutor(
    std::string_view executor_name) {
  {
    std::shared_lock<std::shared_mutex> lck(executor_access_mutex_);
    auto finditr = executor_access_map_.find(executor_name);
    if (finditr != executor_access_map_.end())
      return ibmw::executor::ExecutorHandle(finditr->second->NativeHandle());
  }
  if (state_.load() == State::kShutdown) return ibmw::executor::ExecutorHandle();

  MW_WARN("Get executor failed, executor name '{}'", executor_name);

  return ibmw::executor::ExecutorHandle();
}

void ExecutorBus::RegisterAsioExecutorGenFunc() {
  RegisterExecutorGenFunc("thread_pool", [this]() -> std::unique_ptr<ExecutorBase> {
    auto ptr = std::make_unique<ThreadPoolExecutor>();
    ptr->SetLogger(logger_ptr_);
    return ptr;
  });

  RegisterExecutorGenFunc("strand", [this]() -> std::unique_ptr<ExecutorBase> {
    auto ptr = std::make_unique<StrandExecutor>();
    ptr->SetLogger(logger_ptr_);
    ptr->RegisterGetAsioHandle(
        [this](std::string_view name) -> asio::io_context* {
          auto itr = std::find_if(
              executor_vec_.begin(),
              executor_vec_.end(),
              [name](const std::unique_ptr<ExecutorBase>& executor) -> bool {
                return (executor->Type() == "thread_pool") &&
                       (executor->Name() == name);
              });

          if (itr != executor_vec_.end())
            return dynamic_cast<ThreadPoolExecutor*>(itr->get())->IOCTX();

          return nullptr;
        });
    return ptr;
  });
}

void ExecutorBus::RegisterTBBExecutorGenFunc() {
  RegisterExecutorGenFunc("parallel", [this]() -> std::unique_ptr<ExecutorBase> {
    auto ptr = std::make_unique<ParallelExecutor>();
    ptr->SetLogger(logger_ptr_);
    return ptr;
  });
}

void ExecutorBus::RegisterSimpleThreadExecutorGenFunc() {
  RegisterExecutorGenFunc("dedicated", [this]() -> std::unique_ptr<ExecutorBase> {
    auto ptr = std::make_unique<DedicatedExecutor>();
    ptr->SetLogger(logger_ptr_);
    return ptr;
  });
}

void ExecutorBus::RegisterTimeWheelExecutorGenFunc() {
  RegisterExecutorGenFunc("timer", [this]() -> std::unique_ptr<ExecutorBase> {
    auto ptr = std::make_unique<TimerExecutor>();
    ptr->SetLogger(logger_ptr_);
    ptr->RegisterGetExecutorFunc([this](std::string_view name) {
      return GetExecutor(name);
    });
    return ptr;
  });
}

std::list<std::pair<std::string, std::string>> ExecutorBus::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::string executor_summary;

  // Types line
  std::vector<std::string> executor_type_vec;
  for (const auto& itr : executor_gen_func_map_) {
    executor_type_vec.emplace_back(itr.first);
  }
  executor_summary += "  types: " +
      (executor_type_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(executor_type_vec, ", ")) + "\n";

  // Executor instances table
  if (!executor_vec_.empty()) {
    std::vector<std::vector<std::string>> executor_info_table =
        {{"name", "type", "threadsafe", "timed"}};

    for (const auto& item : executor_vec_) {
      std::vector<std::string> cur_executor_info(4);
      cur_executor_info[0] = item->Name();
      cur_executor_info[1] = item->Type();
      cur_executor_info[2] = item->ThreadSafe() ? "Y" : "N";
      cur_executor_info[3] = item->SupportsTimedDispatch() ? "Y" : "N";
      executor_info_table.emplace_back(std::move(cur_executor_info));
    }

    executor_summary += ibmw::common::util::FormatTree(executor_info_table);
  }

  std::list<std::pair<std::string, std::string>> report{
      {"Executors", executor_summary}};

  for (const auto& driver_ptr : executor_vec_) {
    report.splice(report.end(), driver_ptr->GenInitializationReport());
  }

  return report;
}

}  // namespace ibmw::runtime::core::executor
