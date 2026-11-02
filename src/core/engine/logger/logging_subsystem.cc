// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/logger/logging_subsystem.h"
#include "engine/logger/console_logger_driver.h"
#include "engine/logger/crash_signal_handling.h"
#include "engine/logger/log_level_tool.h"
#include "engine/logger/rotate_file_logger_driver.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::logger::LoggingSubsystem::Options> {
  using Options = ibmw::runtime::core::logger::LoggingSubsystem::Options;

  static Node encode(const Options& rhs) {
    Node node;
    node["core_level"] = ibmw::runtime::core::logger::LogLevelTool::GetLogLevelName(rhs.core_lvl);
    node["default_module_level"] = ibmw::runtime::core::logger::LogLevelTool::GetLogLevelName(rhs.default_module_lvl);
    node["enable_crash_log"] = rhs.enable_crash_log;

    node["backends"] = YAML::Node();
    for (const auto& driver_options : rhs.drivers_options) {
      Node driver_options_node;
      driver_options_node["type"] = driver_options.type;
      driver_options_node["options"] = driver_options.options;
      node["backends"].push_back(driver_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["core_level"]) {
      rhs.core_lvl = ibmw::runtime::core::logger::LogLevelTool::GetLogLevelFromName(
          node["core_level"].as<std::string>());
    }

    if (node["default_module_level"]) {
      rhs.default_module_lvl = ibmw::runtime::core::logger::LogLevelTool::GetLogLevelFromName(
          node["default_module_level"].as<std::string>());
    }

    if (node["enable_crash_log"])
      rhs.enable_crash_log = node["enable_crash_log"].as<bool>();

    if (node["backends"] && node["backends"].IsSequence()) {
      for (const auto& driver_options_node : node["backends"]) {
        auto driver_options = Options::DriverOptions{
            .type = driver_options_node["type"].as<std::string>()};

        if (driver_options_node["options"])
          driver_options.options = driver_options_node["options"];
        else
          driver_options.options = YAML::Node(YAML::NodeType::Null);

        rhs.drivers_options.emplace_back(std::move(driver_options));
      }
    }
    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::logger {

void LoggingSubsystem::Initialize(YAML::Node options_node) {
  RegisterConsoleLoggerDriverGenFunc();
  RegisterRotateFileLoggerDriverGenFunc();

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Logging service can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  // create logger
  for (auto& driver_options : options_.drivers_options) {
    auto finditr = logger_driver_gen_func_map_.find(driver_options.type);
    MW_REQUIRE(finditr != logger_driver_gen_func_map_.end(),
                            "Invalid logger driver type '{}'.",
                            driver_options.type);

    auto logger_driver_ptr = finditr->second();
    MW_REQUIRE(
        logger_driver_ptr,
        "Gen logger driver failed, logger driver type '{}'.",
        driver_options.type);

    if (!logger_driver_ptr->AllowDuplicates()) {
      MW_REQUIRE(
          std::find_if(logger_driver_vec_.begin(), logger_driver_vec_.end(),
                       [type = driver_options.type](const auto& driver_ptr) -> bool {
                         return driver_ptr->Type() == type;
                       }) == logger_driver_vec_.end(),
          "Logger driver type'{}' do not allow duplicate.", driver_options.type);
    }

    logger_driver_ptr->Initialize(driver_options.options);

    logger_driver_vec_.emplace_back(std::move(logger_driver_ptr));
  }

  MW_CHECK_WARN(!logger_driver_vec_.empty(), "No log driver!");

  if (options_.enable_crash_log) {
    crash_signal_handling_.reset();  // restores what it replaced first
    crash_signal_handling_ = std::make_unique<CrashSignalHandling>();
    CrashSignalHandling::SetLogger(logger_ptr_);
    // The drivers write on their log executor: wait (bounded) for the report
    // to be written before the handler ends the process.
    CrashSignalHandling::SetCrashFlush(&crash_flush_hook_);
  } else {
    crash_signal_handling_.reset();
    // The crash flush hook is left alone: Initialize() runs once per object,
    // so this one never published its hook, and another owner's hook (an
    // earlier LoggingSubsystem whose handler and logger are still in place)
    // must stay.
  }

  options_node = options_;

  MW_INFO("[Logging] initialized");
}

void LoggingSubsystem::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (auto& driver : logger_driver_vec_) {
    driver->Start();
  }

  MW_INFO("[Logging] started");
}

void LoggingSubsystem::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Logging] stopped");

  // logger_access_map_ cannot be cleared, some extensions still generate logs

  for (auto& driver : logger_driver_vec_) {
    driver->Shutdown();
  }

  // logger_driver cannot be cleared, there may be unfinished log tasks

  logger_driver_gen_func_map_.clear();

  get_executor_func_ = nullptr;
}

void LoggingSubsystem::Flush() {
  for (auto& driver : logger_driver_vec_) {
    driver->Flush();
  }
}

bool LoggingSubsystem::FlushForCrash(std::chrono::milliseconds timeout) noexcept {
  // One deadline for all drivers, so the total wait is bounded by timeout.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool all_written = true;
  for (auto& driver : logger_driver_vec_) {
    all_written = driver->FlushForCrash(deadline) && all_written;
  }
  return all_written;
}

LoggingSubsystem::~LoggingSubsystem() {
  // The handler stays installed after this object is gone; drop the hook
  // that points to it (if still published) and wait out a running one.
  CrashSignalHandling::ClearCrashFlush(&crash_flush_hook_);
}

void LoggingSubsystem::RegisterGetExecutorFunc(
    const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_executor_func_ = get_executor_func;
}

void LoggingSubsystem::RegisterLoggerDriverGenFunc(
    std::string_view type,
    LoggerDriverGenFunc&& logger_driver_gen_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  logger_driver_gen_func_map_.emplace(type, std::move(logger_driver_gen_func));
}

const LoggerAccess& LoggingSubsystem::GetLoggerAccess(const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  const std::string& real_module_name =
      (module_info.module_name.empty()) ? "core" : module_info.module_name;

  auto itr = logger_access_map_.find(real_module_name);
  if (itr != logger_access_map_.end()) return *(itr->second);

  ibmw_log_level_t log_lvl =
      (real_module_name == "core")
          ? options_.core_lvl
          : (module_info.use_default_log_lvl ? options_.default_module_lvl
                                             : module_info.log_lvl);

  auto emplace_ret = logger_access_map_.emplace(
      real_module_name,
      std::make_unique<LoggerAccess>(real_module_name, log_lvl, logger_driver_vec_));

  return *(emplace_ret.first->second);
}

const LoggerAccess& LoggingSubsystem::GetLoggerAccess(std::string_view logger_name) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  const std::string& real_logger_name =
      (logger_name.empty()) ? "core" : std::string(logger_name);

  auto itr = logger_access_map_.find(real_logger_name);
  if (itr != logger_access_map_.end()) return *(itr->second);

  // use core_lvl
  auto emplace_ret = logger_access_map_.emplace(
      real_logger_name,
      std::make_unique<LoggerAccess>(real_logger_name, options_.core_lvl, logger_driver_vec_));

  return *(emplace_ret.first->second);
}

std::unordered_map<std::string, ibmw_log_level_t> LoggingSubsystem::GetAllLoggerLevels() const {
  MW_REQUIRE(
      state_.load() == State::kStart,
      "Method can only be called when state is 'Start'.");

  std::unordered_map<std::string, ibmw_log_level_t> result;
  for (const auto& itr : logger_access_map_) {
    result.emplace(itr.first, itr.second->LogLevel());
  }
  return result;
}

void LoggingSubsystem::SetLoggerLevels(
    const std::unordered_map<std::string, ibmw_log_level_t>& logger_lvls) {
  MW_REQUIRE(
      state_.load() == State::kStart,
      "Method can only be called when state is 'Start'.");

  for (const auto& itr : logger_lvls) {
    auto find_itr = logger_access_map_.find(itr.first);
    if (find_itr == logger_access_map_.end()) continue;

    find_itr->second->SetLogLevel(itr.second);
  }
}

void LoggingSubsystem::RegisterConsoleLoggerDriverGenFunc() {
  RegisterLoggerDriverGenFunc("console", [this]() -> std::unique_ptr<LoggerDriverBase> {
    auto ptr = std::make_unique<ConsoleLoggerDriver>();
    ptr->RegisterGetExecutorFunc(get_executor_func_);
    return ptr;
  });
}

void LoggingSubsystem::RegisterRotateFileLoggerDriverGenFunc() {
  RegisterLoggerDriverGenFunc("rotate_file", [this]() -> std::unique_ptr<LoggerDriverBase> {
    auto ptr = std::make_unique<RotateFileLoggerDriver>();
    ptr->RegisterGetExecutorFunc(get_executor_func_);
    return ptr;
  });
}

std::list<std::pair<std::string, std::string>> LoggingSubsystem::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::vector<std::string> logger_driver_type_vec;
  logger_driver_type_vec.reserve(logger_driver_vec_.size());
  for (const auto& logger_driver : logger_driver_vec_) {
    logger_driver_type_vec.emplace_back(logger_driver->Type());
  }

  std::string log_driver_summary = "  drivers: " +
      (logger_driver_type_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(logger_driver_type_vec, ", ")) + "\n";

  std::list<std::pair<std::string, std::string>> report{
      {"Logging", log_driver_summary}};

  for (const auto& driver_ptr : logger_driver_vec_) {
    report.splice(report.end(), driver_ptr->GenInitializationReport());
  }

  return report;
}

const std::vector<std::unique_ptr<LoggerDriverBase>>& LoggingSubsystem::GetUsedLoggerDriver() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return logger_driver_vec_;
}
}  // namespace ibmw::runtime::core::logger
