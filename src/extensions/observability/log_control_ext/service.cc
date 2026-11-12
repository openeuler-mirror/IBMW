// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "log_control_ext/service.h"
#include "engine/logger/log_level_tool.h"
#include "log_control_ext/global.h"

namespace ibmw::extensions::log_control_extension {

void LogControlServiceImpl::HandleGetModuleLogLevel(
    const ibmw::protocols::log_control_extension::GetModuleLogLevelReq& req,
    ibmw::protocols::log_control_extension::GetModuleLogLevelRsp& rsp) {
  rsp = {};
  const auto& log_lvl_map = logging_subsystem_ptr_->GetAllLoggerLevels();

  // if empty, then get all modules
  if (req.module_names.empty()) {
    for (const auto& itr : log_lvl_map) {
      rsp.module_log_level_map.emplace(
          itr.first,
          std::string(ibmw::runtime::core::logger::LogLevelTool::GetLogLevelName(itr.second)));
    }
    return;
  }

  for (const auto& module_name : req.module_names) {
    auto finditr = log_lvl_map.find(module_name);
    if (module_name.empty() || finditr == log_lvl_map.end()) {
      rsp.module_log_level_map.clear();
      SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
      return;
    }

    rsp.module_log_level_map.emplace(
        finditr->first,
        std::string(ibmw::runtime::core::logger::LogLevelTool::GetLogLevelName(finditr->second)));
  }
}

void LogControlServiceImpl::HandleSetModuleLogLevel(
    const ibmw::protocols::log_control_extension::SetModuleLogLevelReq& req,
    ibmw::protocols::log_control_extension::SetModuleLogLevelRsp& rsp) {
  rsp = {};
  if (req.module_log_level_map.empty()) {
    SetErrorCode(ErrorCode::kEmptySettings, rsp);
    return;
  }
  const auto levels = logging_subsystem_ptr_->GetAllLoggerLevels();
  std::unordered_map<std::string, ibmw_log_level_t> log_lvl_settings;
  // Validate the entire request before mutating any logger. This is not a
  // concurrent transaction across loggers; individual levels remain atomic.
  for (const auto& [name, value] : req.module_log_level_map) {
    if (name.empty() || !levels.contains(name)) {
      SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
      return;
    }
    const auto level = ibmw::protocols::log_control_extension::NormalizeLogLevel(value);
    if (!level) {
      SetErrorCode(ErrorCode::kInvalidLogLevel, rsp);
      return;
    }
    log_lvl_settings.emplace(name,
        ibmw::runtime::core::logger::LogLevelTool::GetLogLevelFromName(*level));
  }
  logging_subsystem_ptr_->SetLoggerLevels(log_lvl_settings);
}

}  // namespace ibmw::extensions::log_control_extension
