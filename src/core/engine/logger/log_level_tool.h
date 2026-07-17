// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <string_view>

#include "c/logger/logger_base.h"

namespace ibmw::runtime::core::logger {

class LogLevelTool {
 public:
  static std::string_view GetLogLevelName(ibmw_log_level_t lvl) {
    return kLvlNameArray[static_cast<uint32_t>(lvl)];
  }

  static ibmw_log_level_t GetLogLevelFromName(std::string_view lvl_name) {
    for (int ii = 0; ii <= ibmw_log_level_t::IBMW_LOG_LEVEL_OFF; ++ii) {
      if (lvl_name.size() != kLvlNameArray[ii].size()) {
        continue;
      }
#if defined(_WIN32)
      if (_strnicmp(lvl_name.data(), kLvlNameArray[ii].data(), lvl_name.size()) == 0)
        return static_cast<ibmw_log_level_t>(ii);
#else
      if (strncasecmp(lvl_name.data(), kLvlNameArray[ii].data(), lvl_name.size()) == 0)
        return static_cast<ibmw_log_level_t>(ii);
#endif
    }
    return ibmw_log_level_t::IBMW_LOG_LEVEL_OFF;
  }

 private:
  static constexpr std::string_view
      kLvlNameArray[ibmw_log_level_t::IBMW_LOG_LEVEL_OFF + 1] = {
          "Trace", "Debug", "Info", "Warn", "Error", "Fatal", "Off"};
};
}  // namespace ibmw::runtime::core::logger
