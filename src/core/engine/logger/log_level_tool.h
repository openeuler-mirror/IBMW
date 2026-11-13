// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <optional>
#include <string_view>

#include "c/logger/logger_base.h"

namespace ibmw::runtime::core::logger {

class LogLevelTool {
 public:
  static std::string_view GetLogLevelName(ibmw_log_level_t lvl) {
    return kLvlNameArray[static_cast<uint32_t>(lvl)];
  }

  // Unknown names map to Off; use TryGetLogLevelFromName to detect them.
  static ibmw_log_level_t GetLogLevelFromName(std::string_view lvl_name) {
    return TryGetLogLevelFromName(lvl_name).value_or(ibmw_log_level_t::IBMW_LOG_LEVEL_OFF);
  }

  // Case-insensitive; std::nullopt for an unknown name.
  static std::optional<ibmw_log_level_t> TryGetLogLevelFromName(std::string_view lvl_name) {
    for (int ii = 0; ii <= ibmw_log_level_t::IBMW_LOG_LEVEL_OFF; ++ii) {
      if (lvl_name.size() != kLvlNameArray[ii].size()) {
        continue;
      }
      if (strncasecmp(lvl_name.data(), kLvlNameArray[ii].data(), lvl_name.size()) == 0)
        return static_cast<ibmw_log_level_t>(ii);
    }
    return std::nullopt;
  }

 private:
  static constexpr std::string_view
      kLvlNameArray[ibmw_log_level_t::IBMW_LOG_LEVEL_OFF + 1] = {
          "Trace", "Debug", "Info", "Warn", "Error", "Fatal", "Off"};
};
}  // namespace ibmw::runtime::core::logger
