// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/logger/console_logger_driver.h"

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>

#include "engine/logger/formatter.h"
#include "base/exception.h"

#define CC_NONE "\033[0m"
#define CC_BLACK "\033[0;30m"
#define CC_L_BLACK "\033[1;30m"
#define CC_RED "\033[0;31m"
#define CC_L_RED "\033[1;31m"
#define CC_GREEN "\033[0;32m"
#define CC_L_GREEN "\033[1;32m"
#define CC_BROWN "\033[0;33m"
#define CC_YELLOW "\033[1;33m"
#define CC_BLUE "\033[0;34m"
#define CC_L_BLUE "\033[1;34m"
#define CC_PURPLE "\033[0;35m"
#define CC_L_PURPLE "\033[1;35m"
#define CC_CYAN "\033[0;36m"
#define CC_L_CYAN "\033[1;36m"
#define CC_GRAY "\033[0;37m"
#define CC_WHITE "\033[1;37m"

#define CC_BOLD "\033[1m"
#define CC_UNDERLINE "\033[4m"
#define CC_BLINK "\033[5m"
#define CC_REVERSE "\033[7m"
#define CC_HIDE "\033[8m"
#define CC_CLEAR "\033[2J"

#define CC_DBG CC_BLUE
#define CC_INF CC_GREEN
#define CC_WRN CC_YELLOW
#define CC_ERR CC_PURPLE
#define CC_FATAL CC_RED

namespace YAML {
template <>
struct convert<ibmw::runtime::core::logger::ConsoleLoggerDriver::Options> {
  using Options = ibmw::runtime::core::logger::ConsoleLoggerDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;
    node["color"] = rhs.print_color;
    node["module_filter"] = rhs.module_filter;
    node["pattern"] = rhs.pattern;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["color"]) rhs.print_color = node["color"].as<bool>();
    if (node["module_filter"])
      rhs.module_filter = node["module_filter"].as<std::string>();
    if (node["pattern"])
      rhs.pattern = node["pattern"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::logger {

void ConsoleLoggerDriver::Initialize(YAML::Node options_node) {
  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  log_executor_ = get_executor_func_("");  // if input an  empty string , use watchdog_executor
  MW_ASSERT(log_executor_, "Watchdog_executor is invalid.");

  // Pre-compile the module filter regex once.
  try {
    compiled_module_filter_ = std::regex(options_.module_filter, std::regex::ECMAScript | std::regex::optimize);
  } catch (const std::exception& e) {
    fprintf(stderr, "Invalid module_filter regex '%s': %s\n", options_.module_filter.c_str(), e.what());
    compiled_module_filter_ = std::regex("(.*)", std::regex::ECMAScript | std::regex::optimize);
  }

  if (!options_.pattern.empty()) {
    pattern_ = options_.pattern;
  }
  formatter_.SetPattern(pattern_);

  options_node = options_;

  run_flag_.store(true);
}

void ConsoleLoggerDriver::Log(const LogDataWrapper& log_data_wrapper) noexcept {
  try {
    if (!run_flag_.load()) [[unlikely]]
      return;

    if (!CheckLog(log_data_wrapper)) [[unlikely]]
      return;

    std::string log_data_str = formatter_.Format(log_data_wrapper);

    auto log_work = [this, lvl = log_data_wrapper.lvl, log_data_str{std::move(log_data_str)}]() {
      if (options_.print_color) {
        static constexpr std::string_view
            kColorArray[ibmw_log_level_t::IBMW_LOG_LEVEL_OFF] = {
                "", CC_DBG, CC_INF, CC_WRN, CC_ERR, CC_FATAL};

        if (kColorArray[lvl].empty()) {
          std::cout.write(log_data_str.data(), log_data_str.size());
        } else {
          std::cout.write(kColorArray[lvl].data(), kColorArray[lvl].size())
              .write(log_data_str.data(), log_data_str.size())
              .write(CC_NONE, sizeof(CC_NONE) - 1);
        }
      } else {
        std::cout.write(log_data_str.data(), log_data_str.size());
      }
      std::cout << '\n';
      std::cout.flush();
    };

    log_executor_.Dispatch(std::move(log_work));
  } catch (const std::exception& e) {
    fprintf(stderr, "Log get exception: %s\n", e.what());
  }
}

void ConsoleLoggerDriver::Flush() noexcept {
  // Entries are written on the log executor, so wait until the ones queued
  // before this call have run (CORE-015). The barrier owns the only reference
  // to the promise: if the executor drops it, the wait returns at once.
  try {
    if (log_executor_ && !log_executor_.IsInCurrentExecutor()) {
      auto done = std::make_shared<std::promise<void>>();
      auto flushed = done->get_future();
      log_executor_.Dispatch([done = std::move(done)] { done->set_value(); });
      // A stalled log executor must not hold up the shutdown.
      (void)flushed.wait_for(std::chrono::seconds(1));
    }
    std::cout.flush();
  } catch (...) {
  }
}

bool ConsoleLoggerDriver::CheckLog(const LogDataWrapper& log_data_wrapper) {
  {
    std::shared_lock lock(module_filter_map_mutex_);
    auto find_itr = module_filter_map_.find(log_data_wrapper.module_name);
    if (find_itr != module_filter_map_.end()) {
      return find_itr->second;
    }
  }

  bool if_log = false;

  try {
    if (std::regex_match(
            log_data_wrapper.module_name.begin(),
            log_data_wrapper.module_name.end(),
            compiled_module_filter_)) {
      if_log = true;
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "Regex get exception, expr: %s, string: %s, exception info: %s\n",
            options_.module_filter.c_str(), log_data_wrapper.module_name.data(), e.what());
  }

  std::unique_lock lock(module_filter_map_mutex_);
  module_filter_map_.emplace(log_data_wrapper.module_name, if_log);

  return if_log;
}
}  // namespace ibmw::runtime::core::logger
