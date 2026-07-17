// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Consolidated core type definitions: version, topic metadata, module info.

#pragma once

#include <string>

#include "base/ibmw_config.h"
#include "c/logger/logger_base.h"

namespace ibmw::runtime::core::util {

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
constexpr const char* GetIbmwVersion() {
  return IBMW_RUNTIME_VERSION;
}

// ---------------------------------------------------------------------------
// TopicMetaKey — composite key for topic+msg_type lookups
// ---------------------------------------------------------------------------
struct TopicMetaKey {
  std::string topic_name;
  std::string msg_type;

  bool operator==(const TopicMetaKey& rhs) const {
    return topic_name == rhs.topic_name && msg_type == rhs.msg_type;
  }

  struct Hash {
    std::size_t operator()(const TopicMetaKey& k) const {
      return (std::hash<std::string>()(k.topic_name)) ^
             (std::hash<std::string>()(k.msg_type));
    }
  };
};

// ---------------------------------------------------------------------------
// ModuleDetailInfo — per-module metadata
// ---------------------------------------------------------------------------
struct ModuleDetailInfo {
  // base info
  std::string module_name;
  std::string pkg_path;

  // detail info
  uint32_t major_version = 0;
  uint32_t minor_version = 0;
  uint32_t patch_version = 0;
  uint32_t build_version = 0;

  std::string author;
  std::string description;

  // options
  ibmw_log_level_t log_lvl = ibmw_log_level_t::IBMW_LOG_LEVEL_TRACE;
  bool use_default_log_lvl = true;
  std::string cfg_file_path;
};

}  // namespace ibmw::runtime::core::util
