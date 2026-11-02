// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "time_manipulator_ext/service.h"
#include "time_manipulator_ext/global.h"

namespace ibmw::extensions::time_manipulator_extension {

void TimeManipulatorServiceImpl::HandleSetTimeRatio(
    const ibmw::protocols::time_manipulator_extension::SetTimeRatioReq& req,
    ibmw::protocols::time_manipulator_extension::CommonRsp& rsp) {
  auto itr = executor_map_.find(req.executor_name);
  if (itr == executor_map_.end()) [[unlikely]] {
    SetErrorCode(ErrorCode::kInvalidExecutorName, rsp);
    return;
  }

  if (!itr->second->SetTimeRatio(req.time_ratio)) [[unlikely]] {
    SetErrorCode(ErrorCode::kInvalidTimeRatio, rsp);
  }
  rsp.time_ratio = itr->second->GetTimeRatio();
}

void TimeManipulatorServiceImpl::HandlePause(
    const ibmw::protocols::time_manipulator_extension::PauseReq& req,
    ibmw::protocols::time_manipulator_extension::CommonRsp& rsp) {
  auto itr = executor_map_.find(req.executor_name);
  if (itr == executor_map_.end()) [[unlikely]] {
    SetErrorCode(ErrorCode::kInvalidExecutorName, rsp);
    return;
  }

  itr->second->SetTimeRatio(0.0);
  rsp.time_ratio = itr->second->GetTimeRatio();
}

void TimeManipulatorServiceImpl::HandleGetTimeRatio(
    const ibmw::protocols::time_manipulator_extension::GetTimeRatioReq& req,
    ibmw::protocols::time_manipulator_extension::CommonRsp& rsp) {
  auto itr = executor_map_.find(req.executor_name);
  if (itr == executor_map_.end()) [[unlikely]] {
    SetErrorCode(ErrorCode::kInvalidExecutorName, rsp);
    return;
  }

  rsp.time_ratio = itr->second->GetTimeRatio();
}

}  // namespace ibmw::extensions::time_manipulator_extension
