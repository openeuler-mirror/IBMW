// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/executor/executor.h"
#include "engine/isubsystem.h"
#include "engine/executor/executor_access.h"
#include "engine/core_types.h"
#include "base/string_util.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::executor {

class ExecutorBus : public ISubsystem {
 public:
  struct Options {
    struct ExecutorOptions {
      std::string name;
      std::string type;
      YAML::Node options;
    };
    std::vector<ExecutorOptions> executors_options;
  };

  using ExecutorGenFunc = std::function<std::unique_ptr<ExecutorBase>()>;

 public:
  ExecutorBus() = default;
  ~ExecutorBus() override = default;

  ExecutorBus(const ExecutorBus&) = delete;
  ExecutorBus& operator=(const ExecutorBus&) = delete;

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void RegisterExecutorGenFunc(std::string_view type,
                               ExecutorGenFunc&& executor_gen_func);

  const ExecutorBusAccess& GetExecutorAccess(const util::ModuleDetailInfo& module_info);
  const ExecutorBusAccess& GetExecutorAccess(std::string_view module_name = "core") {
    return GetExecutorAccess(
        util::ModuleDetailInfo{.module_name = std::string(module_name), .pkg_path = "core"});
  }

  std::list<std::pair<std::string, std::string>> GenInitializationReport() const override;

  ibmw::executor::ExecutorHandle GetExecutor(std::string_view executor_name);

  void SetUsedExecutorName(std::string_view executor_name) {
    used_executor_names_.emplace(executor_name);
  }

  const std::vector<std::unique_ptr<ExecutorBase>>& GetAllExecutors() const {
    return executor_vec_;
  }

  const std::unordered_map<std::string, ExecutorGenFunc>& GetExecutorGenFuncMap() const {
    return executor_gen_func_map_;
  }

 private:
  void RegisterAsioExecutorGenFunc();
  void RegisterTBBExecutorGenFunc();
  void RegisterSimpleThreadExecutorGenFunc();
  void RegisterTimeWheelExecutorGenFunc();

 private:
  Options options_;

  std::unordered_map<std::string, ExecutorGenFunc> executor_gen_func_map_;

  std::set<std::string> used_executor_names_;

  std::vector<std::unique_ptr<ExecutorBase>> executor_vec_;

  std::unordered_map<
      std::string,
      std::unique_ptr<ExecutorAccess>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      executor_access_map_;

  std::unordered_map<
      std::string,
      std::unique_ptr<ExecutorBusAccess>>
      executor_bus_access_map_;
};

}  // namespace ibmw::runtime::core::executor
