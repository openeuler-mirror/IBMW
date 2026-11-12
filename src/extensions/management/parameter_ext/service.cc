// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "parameter_ext/service.h"

#include <algorithm>
#include "parameter_ext/global.h"

#include <utility>
#include <vector>

namespace ibmw::extensions::parameter_extension {

void ParameterServiceImpl::LogHandlerFailure(const char* what) noexcept {
  try {
    MW_ERROR("Parameter service handler failed: {}", what);
  } catch (...) {
  }
}

void ParameterServiceImpl::HandleSet(
    const ibmw::protocols::parameter_extension::SetParameterReq& req,
    ibmw::protocols::parameter_extension::SetParameterRsp& rsp) {
  auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(req.module_name);
  if (parameter_handle_ptr == nullptr) {
    SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
    return;
  }
  if (req.parameter_key.empty()) {
    SetErrorCode(ErrorCode::kInvalidParameterName, rsp);
    return;
  }
  parameter_handle_ptr->SetParameterValue(req.parameter_key, req.parameter_value);
}

void ParameterServiceImpl::HandleGet(
    const ibmw::protocols::parameter_extension::GetParameterReq& req,
    ibmw::protocols::parameter_extension::GetParameterRsp& rsp) {
  auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(req.module_name);
  if (parameter_handle_ptr == nullptr) {
    SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
    return;
  }

  if (req.parameter_key.empty()) {
    SetErrorCode(ErrorCode::kInvalidParameterName, rsp);
    return;
  }
  auto parameter_ptr = parameter_handle_ptr->GetParameter(req.parameter_key);
  if (parameter_ptr) {
    rsp.parameter_value = *parameter_ptr;
  } else {
    SetErrorCode(ErrorCode::kParameterNotFound, rsp);
  }
}

void ParameterServiceImpl::HandleList(
    const ibmw::protocols::parameter_extension::ListParameterReq& req,
    ibmw::protocols::parameter_extension::ListParameterRsp& rsp) {
  auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(req.module_name);
  if (parameter_handle_ptr == nullptr) {
    SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
    return;
  }

  rsp.parameter_keys = parameter_handle_ptr->ListParameter();
  std::sort(rsp.parameter_keys.begin(), rsp.parameter_keys.end());
}

void ParameterServiceImpl::HandleDump(
    const ibmw::protocols::parameter_extension::DumpParameterReq& req,
    ibmw::protocols::parameter_extension::DumpParameterRsp& rsp) {
  for (const auto& module_name : req.module_names) {
    auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(module_name);

    // Check the legality of module name
    if (parameter_handle_ptr == nullptr) {
      rsp.module_parameter_map.clear();
      SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
      return;
    }

    auto& parameter_map = rsp.module_parameter_map[module_name];

    const auto& parameter_names = parameter_handle_ptr->ListParameter();

    for (const auto& parameter_name : parameter_names) {
      auto parameter_ptr = parameter_handle_ptr->GetParameter(parameter_name);
      if (!parameter_ptr) [[unlikely]] {
        continue;
      }

      parameter_map.value.emplace(parameter_name, *parameter_ptr);
    }
  }
}

void ParameterServiceImpl::HandleLoad(
    const ibmw::protocols::parameter_extension::LoadParameterReq& req,
    ibmw::protocols::parameter_extension::LoadParameterRsp& rsp) {
  // Validate the whole request before applying anything, so that a bad
  // module name or key does not leave the parameters half loaded.
  std::vector<std::pair<ibmw::runtime::core::parameter::ParameterHandle*, const decltype(req.module_parameter_map)::mapped_type*>> targets;
  targets.reserve(req.module_parameter_map.size());
  for (const auto& [module_name, parameter_map] : req.module_parameter_map) {
    auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(module_name);

    // Check the legality of module name
    if (parameter_handle_ptr == nullptr) {
      SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
      return;
    }
    for (const auto& [key, value] : parameter_map.value) {
      if (key.empty()) {
        SetErrorCode(ErrorCode::kInvalidParameterName, rsp);
        return;
      }
    }
    targets.emplace_back(parameter_handle_ptr, &parameter_map);
  }

  for (const auto& [parameter_handle_ptr, parameter_map] : targets) {
    for (const auto& [key, value] : parameter_map->value) {
      parameter_handle_ptr->SetParameter(key, value);
    }
  }
}

void ParameterServiceImpl::HandleListModules(
    const ibmw::protocols::parameter_extension::ListModulesReq&,
    ibmw::protocols::parameter_extension::ListModulesRsp& rsp) {
  rsp.module_names = parameter_store_ptr_->ListModules();
}

}  // namespace ibmw::extensions::parameter_extension
