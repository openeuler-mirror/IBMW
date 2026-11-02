// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_type_support.h"
#include "record_playback_ext/playback_action.h"
#include "record_playback_ext/record_action.h"
#include "record_playback_ext/record_playback_types.h"

namespace ibmw::extensions::record_playback_extension {

/**
 * @brief JSON-based RPC service implementation for record/playback management.
 *
 * Replaces the protobuf-generated RecordPlaybackServiceCoService.
 * Inherits from ibmw::rpc::ServiceBase directly and registers
 * seven JSON-typed RPC handlers in the constructor:
 *   - StartRecord
 *   - StopRecord
 *   - StartPlayback
 *   - StopPlayback
 *   - UpdateMetadata
 *   - UpdateRecordAction
 *   - GetRecordActionStatus
 */
class RecordPlaybackServiceImpl : public ibmw::rpc::ServiceBase {
 public:
  RecordPlaybackServiceImpl()
      : ibmw::rpc::ServiceBase(
            "json",
            "ibmw.protocols.record_playback_extension.RecordPlaybackService") {
    using namespace ibmw::protocols::record_playback_extension;

    // Register StartRecord handler
    RegisterServiceFunc(
        "StartRecord",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<StartRecordReq>(),
        ibmw::json::GetJsonMessageTypeSupport<StartRecordRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<StartRecordRsp*>(rsp);
              InvokeGuarded("StartRecord", typed_rsp, [&] {
                HandleStartRecord(*static_cast<const StartRecordReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register StopRecord handler
    RegisterServiceFunc(
        "StopRecord",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<StopRecordReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<CommonRsp*>(rsp);
              InvokeGuarded("StopRecord", typed_rsp, [&] {
                HandleStopRecord(*static_cast<const StopRecordReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register StartPlayback handler
    RegisterServiceFunc(
        "StartPlayback",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<StartPlaybackReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<CommonRsp*>(rsp);
              InvokeGuarded("StartPlayback", typed_rsp, [&] {
                HandleStartPlayback(*static_cast<const StartPlaybackReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register StopPlayback handler
    RegisterServiceFunc(
        "StopPlayback",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<StopPlaybackReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<CommonRsp*>(rsp);
              InvokeGuarded("StopPlayback", typed_rsp, [&] {
                HandleStopPlayback(*static_cast<const StopPlaybackReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register UpdateMetadata handler
    RegisterServiceFunc(
        "UpdateMetadata",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<UpdateMetadataReq>(),
        ibmw::json::GetJsonMessageTypeSupport<CommonRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<CommonRsp*>(rsp);
              InvokeGuarded("UpdateMetadata", typed_rsp, [&] {
                HandleUpdateMetadata(*static_cast<const UpdateMetadataReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register UpdateRecordAction handler
    RegisterServiceFunc(
        "UpdateRecordAction",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<UpdateRecordActionReq>(),
        ibmw::json::GetJsonMessageTypeSupport<UpdateRecordActionRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<UpdateRecordActionRsp*>(rsp);
              InvokeGuarded("UpdateRecordAction", typed_rsp, [&] {
                HandleUpdateRecordAction(*static_cast<const UpdateRecordActionReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));

    // Register GetRecordActionStatus handler
    RegisterServiceFunc(
        "GetRecordActionStatus",
        nullptr,
        ibmw::json::GetJsonMessageTypeSupport<GetRecordActionStatusReq>(),
        ibmw::json::GetJsonMessageTypeSupport<GetRecordActionStatusRsp>(),
        ibmw::rpc::ServiceFunc(
            [this](const ibmw_service_context_base_t* ctx_ptr,
                   const void* req, void* rsp,
                   ibmw_function_base_t* callback) {
              auto& typed_rsp = *static_cast<GetRecordActionStatusRsp*>(rsp);
              InvokeGuarded("GetRecordActionStatus", typed_rsp, [&] {
                HandleGetRecordActionStatus(*static_cast<const GetRecordActionStatusReq*>(req), typed_rsp);
              });
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));
  }

  ~RecordPlaybackServiceImpl() override = default;

  // Limits of one UpdateMetadata request (values are parsed as YAML).
  static constexpr size_t kMaxMetadataPairs = 256;
  static constexpr size_t kMaxMetadataKeySize = 256;
  static constexpr size_t kMaxMetadataValueSize = 64 * 1024;

  void SetRecordActionMap(
      std::unordered_map<std::string_view, std::unique_ptr<RecordAction>>* record_action_map_ptr) {
    record_action_map_ptr_ = record_action_map_ptr;
  }

  void SetPlaybackActionMap(
      std::unordered_map<std::string_view, std::unique_ptr<PlaybackAction>>* playback_action_map_ptr) {
    playback_action_map_ptr_ = playback_action_map_ptr;
  }

 private:
  void HandleStartRecord(
      const ibmw::protocols::record_playback_extension::StartRecordReq& req,
      ibmw::protocols::record_playback_extension::StartRecordRsp& rsp);

  void HandleStopRecord(
      const ibmw::protocols::record_playback_extension::StopRecordReq& req,
      ibmw::protocols::record_playback_extension::CommonRsp& rsp);

  void HandleStartPlayback(
      const ibmw::protocols::record_playback_extension::StartPlaybackReq& req,
      ibmw::protocols::record_playback_extension::CommonRsp& rsp);

  void HandleStopPlayback(
      const ibmw::protocols::record_playback_extension::StopPlaybackReq& req,
      ibmw::protocols::record_playback_extension::CommonRsp& rsp);

  void HandleUpdateMetadata(
      const ibmw::protocols::record_playback_extension::UpdateMetadataReq& req,
      ibmw::protocols::record_playback_extension::CommonRsp& rsp);

  void HandleUpdateRecordAction(
      const ibmw::protocols::record_playback_extension::UpdateRecordActionReq& req,
      ibmw::protocols::record_playback_extension::UpdateRecordActionRsp& rsp);

  void HandleGetRecordActionStatus(
      const ibmw::protocols::record_playback_extension::GetRecordActionStatusReq& req,
      ibmw::protocols::record_playback_extension::GetRecordActionStatusRsp& rsp);

  enum class ErrorCode : uint32_t {
    kSuc = 0,
    kInvalidActionName = 1,
    kInvalidActionMode = 2,
    kStartRecordFailed = 3,
    kStartPlaybackFailed = 4,
    kStopRecordFailed = 5,
    kInvalidArgument = 6,
    kTopicNotFound = 7,
    kActionNotRunning = 8,
    kInternalError = 9,
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_ACTION_NAME",
      "INVALID_ACTION_MODE",
      "START_RECORD_FAILED",
      "START_PLAYBACK_FAILED",
      "STOP_RECORD_FAILED",
      "INVALID_ARGUMENT",
      "TOPIC_NOT_FOUND",
      "ACTION_NOT_RUNNING",
      "INTERNAL_ERROR"};

  // Handlers must not throw into the RPC layer: the response callback would
  // never run. An escaped exception becomes INTERNAL_ERROR.
  template <typename Rsp, typename F>
  void InvokeGuarded(std::string_view func_name, Rsp& rsp, F&& handler) noexcept {
    try {
      handler();
    } catch (...) {
      if constexpr (requires { rsp.common_rsp; }) {
        ReportHandlerException(func_name, rsp.common_rsp);
      } else {
        ReportHandlerException(func_name, rsp);
      }
    }
  }

  // Called from a catch block; logs the current exception.
  void ReportHandlerException(
      std::string_view func_name,
      ibmw::protocols::record_playback_extension::CommonRsp& rsp) noexcept;

  template <typename T>
  void SetErrorCode(ErrorCode code, T& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  std::unordered_map<std::string_view, std::unique_ptr<RecordAction>>* record_action_map_ptr_ = nullptr;
  std::unordered_map<std::string_view, std::unique_ptr<PlaybackAction>>* playback_action_map_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::record_playback_extension
