// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "schema/extensions/common/header_types.h"

namespace ibmw::protocols::topic_logger {

/**
 * @brief Log level definitions
 */
enum class LogLevel : uint32_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kFatal = 5,
  kOff = 6,
};

NLOHMANN_JSON_SERIALIZE_ENUM(LogLevel, {
    {LogLevel::kTrace, "TRACE"},
    {LogLevel::kDebug, "DEBUG"},
    {LogLevel::kInfo, "INFO"},
    {LogLevel::kWarn, "WARN"},
    {LogLevel::kError, "ERROR"},
    {LogLevel::kFatal, "FATAL"},
    {LogLevel::kOff, "OFF"},
})

/**
 * @brief Represents a single log entry
 */
struct SingleLogData {
  static constexpr const char* kTypeName =
      "ibmw.protocols.topic_logger.SingleLogData";

  std::string module_name;
  uint64_t thread_id = 0;
  uint64_t time_point = 0;
  LogLevel level = LogLevel::kTrace;
  uint32_t line = 0;
  std::string file_name;
  std::string function_name;
  std::string message;
};

inline void to_json(nlohmann::json& j, const SingleLogData& v) {
  j = nlohmann::json{
      {"module_name", v.module_name},
      {"thread_id", v.thread_id},
      {"time_point", v.time_point},
      {"level", v.level},
      {"line", v.line},
      {"file_name", v.file_name},
      {"function_name", v.function_name},
      {"message", v.message}};
}

inline void from_json(const nlohmann::json& j, SingleLogData& v) {
  if (j.contains("module_name")) j.at("module_name").get_to(v.module_name);
  if (j.contains("thread_id")) j.at("thread_id").get_to(v.thread_id);
  if (j.contains("time_point")) j.at("time_point").get_to(v.time_point);
  if (j.contains("level")) j.at("level").get_to(v.level);
  if (j.contains("line")) j.at("line").get_to(v.line);
  if (j.contains("file_name")) j.at("file_name").get_to(v.file_name);
  if (j.contains("function_name")) j.at("function_name").get_to(v.function_name);
  if (j.contains("message")) j.at("message").get_to(v.message);
}

/**
 * @brief Log data frame containing multiple entries
 */
struct LogData {
  static constexpr const char* kTypeName =
      "ibmw.protocols.topic_logger.LogData";

  ibmw::protocols::common::Header header;
  std::vector<SingleLogData> logs;
  uint64_t dropped_count = 0;
};

inline void to_json(nlohmann::json& j, const LogData& v) {
  j = nlohmann::json{
      {"header", v.header},
      {"logs", v.logs},
      {"dropped_count", v.dropped_count}};
}

inline void from_json(const nlohmann::json& j, LogData& v) {
  if (j.contains("header")) j.at("header").get_to(v.header);
  if (j.contains("logs")) j.at("logs").get_to(v.logs);
  if (j.contains("dropped_count")) j.at("dropped_count").get_to(v.dropped_count);
}

}  // namespace ibmw::protocols::topic_logger
