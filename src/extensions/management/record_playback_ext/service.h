// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
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
              HandleStartRecord(
                  *static_cast<const StartRecordReq*>(req),
                  *static_cast<StartRecordRsp*>(rsp));
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
              HandleStopRecord(
                  *static_cast<const StopRecordReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
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
              HandleStartPlayback(
                  *static_cast<const StartPlaybackReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
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
              HandleStopPlayback(
                  *static_cast<const StopPlaybackReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
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
              HandleUpdateMetadata(
                  *static_cast<const UpdateMetadataReq*>(req),
                  *static_cast<CommonRsp*>(rsp));
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
              HandleUpdateRecordAction(
                  *static_cast<const UpdateRecordActionReq*>(req),
                  *static_cast<UpdateRecordActionRsp*>(rsp));
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
              HandleGetRecordActionStatus(
                  *static_cast<const GetRecordActionStatusReq*>(req),
                  *static_cast<GetRecordActionStatusRsp*>(rsp));
              auto ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
              ops->invoker(callback->object_buf, 0);
            }));
  }

  ~RecordPlaybackServiceImpl() override = default;

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
  };

  static constexpr std::string_view kErrorInfoArray[] = {
      "",
      "INVALID_ACTION_NAME",
      "INVALID_ACTION_MODE",
      "START_RECORD_FAILED",
      "START_PLAYBACK_FAILED",
      "STOP_RECORD_FAILED"};

  template <typename T>
  void SetErrorCode(ErrorCode code, T& rsp) {
    rsp.code = static_cast<uint32_t>(code);
    rsp.msg = std::string(kErrorInfoArray[static_cast<uint32_t>(code)]);
  }

  std::unordered_map<std::string_view, std::unique_ptr<RecordAction>>* record_action_map_ptr_ = nullptr;
  std::unordered_map<std::string_view, std::unique_ptr<PlaybackAction>>* playback_action_map_ptr_ = nullptr;
};

}  // namespace ibmw::extensions::record_playback_extension
