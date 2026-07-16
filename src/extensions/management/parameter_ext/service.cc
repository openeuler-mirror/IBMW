// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "parameter_ext/service.h"
#include "parameter_ext/global.h"

namespace ibmw::extensions::parameter_extension {

void ParameterServiceImpl::HandleSet(
    const ibmw::protocols::parameter_extension::SetParameterReq& req,
    ibmw::protocols::parameter_extension::SetParameterRsp& rsp) {
  auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(req.module_name);
  if (parameter_handle_ptr == nullptr) {
    SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
    return;
  }

  parameter_handle_ptr->SetParameter(req.parameter_key, req.parameter_value);
}

void ParameterServiceImpl::HandleGet(
    const ibmw::protocols::parameter_extension::GetParameterReq& req,
    ibmw::protocols::parameter_extension::GetParameterRsp& rsp) {
  auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(req.module_name);
  if (parameter_handle_ptr == nullptr) {
    SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
    return;
  }

  auto parameter_ptr = parameter_handle_ptr->GetParameter(req.parameter_key);
  if (parameter_ptr) {
    rsp.parameter_value = *parameter_ptr;
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
}

void ParameterServiceImpl::HandleDump(
    const ibmw::protocols::parameter_extension::DumpParameterReq& req,
    ibmw::protocols::parameter_extension::DumpParameterRsp& rsp) {
  for (const auto& module_name : req.module_names) {
    auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(module_name);

    // Check the legality of module name
    if (parameter_handle_ptr == nullptr) {
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
  for (const auto& [module_name, parameter_map] : req.module_parameter_map) {
    auto* parameter_handle_ptr = parameter_store_ptr_->GetParams(module_name);

    // Check the legality of module name
    if (parameter_handle_ptr == nullptr) {
      SetErrorCode(ErrorCode::kInvalidModuleName, rsp);
      return;
    }

    for (const auto& [key, value] : parameter_map.value) {
      parameter_handle_ptr->SetParameter(key, value);
    }
  }
}

}  // namespace ibmw::extensions::parameter_extension
