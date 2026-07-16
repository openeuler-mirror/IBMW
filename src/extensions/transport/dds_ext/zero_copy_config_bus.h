// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

#include "dds_ext/loaned_message.h"

namespace ibmw::extensions::dds_extension {

/**
 * @brief Zero-copy configuration bus
 *
 * Loads and manages zero-copy config at global and per-topic levels.
 */
class ZeroCopyConfigBus {
 public:
  ZeroCopyConfigBus() = default;
  ~ZeroCopyConfigBus() = default;

  bool LoadGlobalConfig(const YAML::Node& config) {
    if (!config["zero_copy"]) return true;

    auto zc = config["zero_copy"];

    if (zc["mode"])
      global_config_.mode = ParseMode(zc["mode"].as<std::string>());
    if (zc["log_level"])
      global_config_.log_level = ParseLogLevel(zc["log_level"].as<std::string>());
    if (zc["monitoring"])
      global_config_.monitoring_enabled = zc["monitoring"].as<bool>();
    if (zc["monitoring_interval"])
      global_config_.monitoring_interval_sec = zc["monitoring_interval"].as<uint32_t>();

    return true;
  }

  bool LoadTopicConfig(const std::string& topic_name, const YAML::Node& config) {
    if (!config["zero_copy"]) return true;

    auto zc = config["zero_copy"];
    ZeroCopyConfig topic_config = global_config_;

    if (zc["mode"])
      topic_config.mode = ParseMode(zc["mode"].as<std::string>());
    if (zc["log_level"])
      topic_config.log_level = ParseLogLevel(zc["log_level"].as<std::string>());
    if (zc["monitoring"])
      topic_config.monitoring_enabled = zc["monitoring"].as<bool>();
    if (zc["monitoring_interval"])
      topic_config.monitoring_interval_sec = zc["monitoring_interval"].as<uint32_t>();

    topic_configs_[topic_name] = topic_config;
    return true;
  }

  const ZeroCopyConfig& GetConfig(const std::string& topic_name) const {
    auto it = topic_configs_.find(topic_name);
    if (it != topic_configs_.end()) return it->second;
    return global_config_;
  }

  const ZeroCopyConfig& GetGlobalConfig() const { return global_config_; }

  void SetGlobalConfig(const ZeroCopyConfig& config) { global_config_ = config; }
  void SetTopicConfig(const std::string& topic_name, const ZeroCopyConfig& config) {
    topic_configs_[topic_name] = config;
  }

  void Clear() {
    global_config_ = ZeroCopyConfig::Auto();
    topic_configs_.clear();
  }

 private:
  ZeroCopyConfig::Mode ParseMode(const std::string& mode_str) {
    if (mode_str == "auto") return ZeroCopyConfig::Mode::AUTO;
    if (mode_str == "strict") return ZeroCopyConfig::Mode::STRICT;
    if (mode_str == "compat") return ZeroCopyConfig::Mode::COMPAT;
    return ZeroCopyConfig::Mode::AUTO;
  }

  ZeroCopyConfig::LogLevel ParseLogLevel(const std::string& level_str) {
    if (level_str == "debug") return ZeroCopyConfig::LogLevel::DEBUG;
    if (level_str == "info") return ZeroCopyConfig::LogLevel::INFO;
    if (level_str == "warn") return ZeroCopyConfig::LogLevel::WARN;
    if (level_str == "error") return ZeroCopyConfig::LogLevel::ERROR;
    if (level_str == "off") return ZeroCopyConfig::LogLevel::OFF;
    return ZeroCopyConfig::LogLevel::WARN;
  }

 private:
  ZeroCopyConfig global_config_ = ZeroCopyConfig::Auto();
  std::unordered_map<std::string, ZeroCopyConfig> topic_configs_;
};

}  // namespace ibmw::extensions::dds_extension
