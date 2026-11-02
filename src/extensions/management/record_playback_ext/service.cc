// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/service.h"
#include "record_playback_ext/global.h"
#include "record_playback_ext/topic_meta.h"

#include <exception>

#include <nlohmann/json.hpp>

namespace ibmw::extensions::record_playback_extension {

void RecordPlaybackServiceImpl::ReportHandlerException(
    std::string_view func_name,
    ibmw::protocols::record_playback_extension::CommonRsp& rsp) noexcept {
  try {
    try {
      throw;
    } catch (const std::exception& e) {
      MW_ERROR("Record playback service '{}' failed: {}", func_name, e.what());
    } catch (...) {
      MW_ERROR("Record playback service '{}' failed: unknown exception", func_name);
    }
    SetErrorCode(ErrorCode::kInternalError, rsp);
  } catch (...) {
  }
}

void RecordPlaybackServiceImpl::HandleStartRecord(
    const ibmw::protocols::record_playback_extension::StartRecordReq& req,
    ibmw::protocols::record_playback_extension::StartRecordRsp& rsp) {
  auto finditr = record_action_map_ptr_->find(req.action_name);
  auto& common_rsp = rsp.common_rsp;

  if (finditr == record_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, common_rsp);
    return;
  }

  auto& action_wrapper = *(finditr->second);

  if (action_wrapper.GetOptions().mode != RecordAction::Options::Mode::kSignal) {
    SetErrorCode(ErrorCode::kInvalidActionMode, common_rsp);
    return;
  }

  uint64_t preparation_duration_s = req.preparation_duration_s;
  uint64_t record_duration_s = req.record_duration_s;

  std::string filefolder;
  bool ret = action_wrapper.StartSignalRecord(preparation_duration_s, record_duration_s, filefolder);
  if (!ret) {
    SetErrorCode(ErrorCode::kStartRecordFailed, common_rsp);
    return;
  }

  rsp.filefolder = filefolder;
}

void RecordPlaybackServiceImpl::HandleStopRecord(
    const ibmw::protocols::record_playback_extension::StopRecordReq& req,
    ibmw::protocols::record_playback_extension::CommonRsp& rsp) {
  auto finditr = record_action_map_ptr_->find(req.action_name);
  if (finditr == record_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, rsp);
    return;
  }

  auto& action_wrapper = *(finditr->second);

  if (action_wrapper.GetOptions().mode != RecordAction::Options::Mode::kSignal) {
    SetErrorCode(ErrorCode::kInvalidActionMode, rsp);
    return;
  }

  bool ret = action_wrapper.StopSignalRecord();
  if (!ret) {
    SetErrorCode(ErrorCode::kStopRecordFailed, rsp);
    return;
  }
}

void RecordPlaybackServiceImpl::HandleStartPlayback(
    const ibmw::protocols::record_playback_extension::StartPlaybackReq& req,
    ibmw::protocols::record_playback_extension::CommonRsp& rsp) {
  auto finditr = playback_action_map_ptr_->find(req.action_name);
  if (finditr == playback_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, rsp);
    return;
  }

  auto& action_wrapper = *(finditr->second);

  if (action_wrapper.GetOptions().mode != PlaybackAction::Options::Mode::kSignal) {
    SetErrorCode(ErrorCode::kInvalidActionMode, rsp);
    return;
  }

  uint64_t skip_duration_s = req.skip_duration_s;
  uint64_t play_duration_s = req.play_duration_s;

  bool ret = action_wrapper.StartSignalPlayback(skip_duration_s, play_duration_s);
  if (!ret) {
    SetErrorCode(ErrorCode::kStartPlaybackFailed, rsp);
    return;
  }
}

void RecordPlaybackServiceImpl::HandleStopPlayback(
    const ibmw::protocols::record_playback_extension::StopPlaybackReq& req,
    ibmw::protocols::record_playback_extension::CommonRsp& rsp) {
  auto finditr = playback_action_map_ptr_->find(req.action_name);
  if (finditr == playback_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, rsp);
    return;
  }

  auto& action_wrapper = *(finditr->second);

  if (action_wrapper.GetOptions().mode != PlaybackAction::Options::Mode::kSignal) {
    SetErrorCode(ErrorCode::kInvalidActionMode, rsp);
    return;
  }

  action_wrapper.StopSignalPlayback();
}

void RecordPlaybackServiceImpl::HandleUpdateMetadata(
    const ibmw::protocols::record_playback_extension::UpdateMetadataReq& req,
    ibmw::protocols::record_playback_extension::CommonRsp& rsp) {
  auto finditr = record_action_map_ptr_->find(req.action_name);
  if (finditr == record_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, rsp);
    return;
  }
  if (req.kv_pairs.size() > kMaxMetadataPairs) {
    SetErrorCode(ErrorCode::kInvalidArgument, rsp);
    return;
  }
  for (const auto& [key, value] : req.kv_pairs) {
    if (key.size() > kMaxMetadataKeySize || value.size() > kMaxMetadataValueSize) {
      SetErrorCode(ErrorCode::kInvalidArgument, rsp);
      return;
    }
  }
  std::unordered_map<std::string, std::string> kv_pairs;
  for (const auto& [key, value] : req.kv_pairs) {
    if (key.empty()) [[unlikely]] {
      MW_WARN("Received metadata update with empty key. Skipping.");
      continue;
    }
    kv_pairs[key] = value;
  }

  auto& action_wrapper = *(finditr->second);

  action_wrapper.UpdateMetadata(std::move(kv_pairs));
}

void RecordPlaybackServiceImpl::HandleUpdateRecordAction(
    const ibmw::protocols::record_playback_extension::UpdateRecordActionReq& req,
    ibmw::protocols::record_playback_extension::UpdateRecordActionRsp& rsp) {
  // Invalid UTF-8 in the request must not make dump() throw.
  MW_INFO("Received update record action request: {}",
          nlohmann::json(req).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
  auto finditr = record_action_map_ptr_->find(req.action_name);
  auto& common_rsp = rsp.common_rsp;
  if (finditr == record_action_map_ptr_->end()) {
    SetErrorCode(ErrorCode::kInvalidActionName, common_rsp);
    return;
  }

  auto& action_wrapper = *(finditr->second);
  std::vector<TopicMeta> topic_metas;
  topic_metas.reserve(req.topic_metas.size());
  for (const auto& topic_meta : req.topic_metas) {
    if (!RecordAction::IsValidSampleFreq(topic_meta.sample_freq.value_or(0))) {
      SetErrorCode(ErrorCode::kInvalidArgument, common_rsp);
      return;
    }
    topic_metas.emplace_back(TopicMeta{
        .topic_name = topic_meta.topic_name,
        .msg_type = topic_meta.msg_type,
        .record_enabled = topic_meta.record_enabled,
        .sample_freq = topic_meta.sample_freq.has_value() ? topic_meta.sample_freq.value() : 0});
  }

  switch (action_wrapper.UpdateTopicMetaRecord(std::move(topic_metas), req.record_enabled)) {
    case RecordAction::UpdateResult::kOk:
      SetErrorCode(ErrorCode::kSuc, common_rsp);
      break;
    case RecordAction::UpdateResult::kTopicNotFound:
      SetErrorCode(ErrorCode::kTopicNotFound, common_rsp);
      break;
    case RecordAction::UpdateResult::kNotRunning:
      SetErrorCode(ErrorCode::kActionNotRunning, common_rsp);
      break;
  }
}

void RecordPlaybackServiceImpl::HandleGetRecordActionStatus(
    const ibmw::protocols::record_playback_extension::GetRecordActionStatusReq& req,
    ibmw::protocols::record_playback_extension::GetRecordActionStatusRsp& rsp) {
  auto& common_rsp = rsp.common_rsp;

  std::vector<std::pair<std::string_view, RecordAction*>> action_vec;

  // If action_names is empty, then return all record actions' status.
  if (req.action_names.empty()) {
    action_vec.reserve(record_action_map_ptr_->size());
    for (auto& itr : *record_action_map_ptr_) {
      action_vec.emplace_back(itr.first, itr.second.get());
    }

    std::sort(
        action_vec.begin(), action_vec.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

  } else {
    action_vec.reserve(req.action_names.size());
    for (const auto& action_name : req.action_names) {
      auto finditr = record_action_map_ptr_->find(action_name);
      if (finditr == record_action_map_ptr_->end()) {
        SetErrorCode(ErrorCode::kInvalidActionName, common_rsp);
        return;
      }

      action_vec.emplace_back(std::string_view(action_name), finditr->second.get());
    }
  }

  rsp.record_action_status_list.reserve(action_vec.size());

  std::vector<RecordAction::StatusSnapshot> snapshot_vec(action_vec.size());

  ibmw::util::DynamicLatch latch;
  for (size_t i = 0; i < action_vec.size(); ++i) {
    action_vec[i].second->GetStatusSnapshot(snapshot_vec[i], latch);
  }
  latch.CloseAndWait();

  for (size_t i = 0; i < action_vec.size(); ++i) {
    const auto& action_name = action_vec[i].first;
    const auto& snapshot = snapshot_vec[i];

    rsp.record_action_status_list.emplace_back();
    auto& status = rsp.record_action_status_list.back();
    status.action_name = std::string(action_name);
    status.record_enabled = snapshot.record_enabled;

    status.topic_meta_list.reserve(snapshot.topic_meta_list.size());
    for (const auto& topic_meta : snapshot.topic_meta_list) {
      ibmw::protocols::record_playback_extension::ProtoTopicMeta proto_topic_meta;
      proto_topic_meta.topic_name = topic_meta.topic_name;
      proto_topic_meta.msg_type = topic_meta.msg_type;
      proto_topic_meta.record_enabled = topic_meta.record_enabled;
      proto_topic_meta.sample_freq = topic_meta.sample_freq;
      status.topic_meta_list.emplace_back(std::move(proto_topic_meta));
    }
  }

  SetErrorCode(ErrorCode::kSuc, common_rsp);
}

}  // namespace ibmw::extensions::record_playback_extension
