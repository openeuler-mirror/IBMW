// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "log_control_ext/service.h"
#include "engine/logger/log_level_tool.h"
#include "log_control_ext/global.h"

namespace ibmw::extensions::log_control_extension {

void LogControlServiceImpl::HandleGetModuleLogLevel(
    const ibmw::protocols::log_control_extension::GetModuleLogLevelReq& req,
    ibmw::protocols::log_control_extension::GetModuleLogLevelRsp& rsp) {
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
    if (finditr == log_lvl_map.end()) {
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
  std::unordered_map<std::string, ibmw_log_level_t> log_lvl_settings;

  for (const auto& itr : req.module_log_level_map) {
    log_lvl_settings.emplace(
        itr.first,
        ibmw::runtime::core::logger::LogLevelTool::GetLogLevelFromName(itr.second));
  }

  logging_subsystem_ptr_->SetLoggerLevels(log_lvl_settings);
}

}  // namespace ibmw::extensions::log_control_extension
