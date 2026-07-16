// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ibmw::protocols::record_playback_extension {

// --- CommonRsp ---

struct CommonRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.CommonRsp";

  uint32_t code = 0;
  std::string msg;
};

inline void to_json(nlohmann::json& j, const CommonRsp& v) {
  j = nlohmann::json{{"code", v.code}, {"msg", v.msg}};
}

inline void from_json(const nlohmann::json& j, CommonRsp& v) {
  if (j.contains("code")) j.at("code").get_to(v.code);
  if (j.contains("msg")) j.at("msg").get_to(v.msg);
}

// --- StartRecordReq ---

struct StartRecordReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.StartRecordReq";

  std::string action_name;
  uint32_t preparation_duration_s = 0;
  uint32_t record_duration_s = 0;  // record forever if value is 0
};

inline void to_json(nlohmann::json& j, const StartRecordReq& v) {
  j = nlohmann::json{
      {"action_name", v.action_name},
      {"preparation_duration_s", v.preparation_duration_s},
      {"record_duration_s", v.record_duration_s}};
}

inline void from_json(const nlohmann::json& j, StartRecordReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
  if (j.contains("preparation_duration_s")) j.at("preparation_duration_s").get_to(v.preparation_duration_s);
  if (j.contains("record_duration_s")) j.at("record_duration_s").get_to(v.record_duration_s);
}

// --- StartRecordRsp ---

struct StartRecordRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.StartRecordRsp";

  CommonRsp common_rsp;
  std::string filefolder;
};

inline void to_json(nlohmann::json& j, const StartRecordRsp& v) {
  j = nlohmann::json{
      {"common_rsp", v.common_rsp},
      {"filefolder", v.filefolder}};
}

inline void from_json(const nlohmann::json& j, StartRecordRsp& v) {
  if (j.contains("common_rsp")) j.at("common_rsp").get_to(v.common_rsp);
  if (j.contains("filefolder")) j.at("filefolder").get_to(v.filefolder);
}

// --- StopRecordReq ---

struct StopRecordReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.StopRecordReq";

  std::string action_name;
};

inline void to_json(nlohmann::json& j, const StopRecordReq& v) {
  j = nlohmann::json{{"action_name", v.action_name}};
}

inline void from_json(const nlohmann::json& j, StopRecordReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
}

// --- StartPlaybackReq ---

struct StartPlaybackReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.StartPlaybackReq";

  std::string action_name;
  uint32_t skip_duration_s = 0;
  uint32_t play_duration_s = 0;  // playback to end if value is 0
};

inline void to_json(nlohmann::json& j, const StartPlaybackReq& v) {
  j = nlohmann::json{
      {"action_name", v.action_name},
      {"skip_duration_s", v.skip_duration_s},
      {"play_duration_s", v.play_duration_s}};
}

inline void from_json(const nlohmann::json& j, StartPlaybackReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
  if (j.contains("skip_duration_s")) j.at("skip_duration_s").get_to(v.skip_duration_s);
  if (j.contains("play_duration_s")) j.at("play_duration_s").get_to(v.play_duration_s);
}

// --- StopPlaybackReq ---

struct StopPlaybackReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.StopPlaybackReq";

  std::string action_name;
};

inline void to_json(nlohmann::json& j, const StopPlaybackReq& v) {
  j = nlohmann::json{{"action_name", v.action_name}};
}

inline void from_json(const nlohmann::json& j, StopPlaybackReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
}

// --- UpdateMetadataReq ---

struct UpdateMetadataReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.UpdateMetadataReq";

  std::string action_name;
  std::map<std::string, std::string> kv_pairs;
};

inline void to_json(nlohmann::json& j, const UpdateMetadataReq& v) {
  j = nlohmann::json{
      {"action_name", v.action_name},
      {"kv_pairs", v.kv_pairs}};
}

inline void from_json(const nlohmann::json& j, UpdateMetadataReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
  if (j.contains("kv_pairs")) j.at("kv_pairs").get_to(v.kv_pairs);
}

// --- ProtoTopicMeta (proto version — different from internal TopicMeta) ---

struct ProtoTopicMeta {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.TopicMeta";

  std::string topic_name;
  std::string msg_type;
  bool record_enabled = false;
  std::optional<double> sample_freq;
};

inline void to_json(nlohmann::json& j, const ProtoTopicMeta& v) {
  j = nlohmann::json{
      {"topic", v.topic_name},
      {"msg_type", v.msg_type},
      {"record_enabled", v.record_enabled}};
  if (v.sample_freq.has_value()) {
    j["sample_freq"] = v.sample_freq.value();
  }
}

inline void from_json(const nlohmann::json& j, ProtoTopicMeta& v) {
  if (j.contains("topic")) j.at("topic").get_to(v.topic_name);
  if (j.contains("msg_type")) j.at("msg_type").get_to(v.msg_type);
  if (j.contains("record_enabled")) j.at("record_enabled").get_to(v.record_enabled);
  if (j.contains("sample_freq")) v.sample_freq = j.at("sample_freq").get<double>();
}

// --- UpdateRecordActionReq ---

struct UpdateRecordActionReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.UpdateRecordActionReq";

  std::string action_name;
  std::vector<ProtoTopicMeta> topic_metas;
  std::optional<bool> record_enabled;
};

inline void to_json(nlohmann::json& j, const UpdateRecordActionReq& v) {
  j = nlohmann::json{
      {"action_name", v.action_name},
      {"topic_metas", v.topic_metas}};
  if (v.record_enabled.has_value()) {
    j["record_enabled"] = v.record_enabled.value();
  }
}

inline void from_json(const nlohmann::json& j, UpdateRecordActionReq& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
  if (j.contains("topic_metas")) j.at("topic_metas").get_to(v.topic_metas);
  if (j.contains("record_enabled")) v.record_enabled = j.at("record_enabled").get<bool>();
}

// --- UpdateRecordActionRsp ---

struct UpdateRecordActionRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.UpdateRecordActionRsp";

  CommonRsp common_rsp;
};

inline void to_json(nlohmann::json& j, const UpdateRecordActionRsp& v) {
  j = nlohmann::json{{"common_rsp", v.common_rsp}};
}

inline void from_json(const nlohmann::json& j, UpdateRecordActionRsp& v) {
  if (j.contains("common_rsp")) j.at("common_rsp").get_to(v.common_rsp);
}

// --- RecordActionStatus ---

struct RecordActionStatus {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.RecordActionStatus";

  std::string action_name;
  bool record_enabled = false;
  std::vector<ProtoTopicMeta> topic_meta_list;
};

inline void to_json(nlohmann::json& j, const RecordActionStatus& v) {
  j = nlohmann::json{
      {"action_name", v.action_name},
      {"record_enabled", v.record_enabled},
      {"topic_meta_list", v.topic_meta_list}};
}

inline void from_json(const nlohmann::json& j, RecordActionStatus& v) {
  if (j.contains("action_name")) j.at("action_name").get_to(v.action_name);
  if (j.contains("record_enabled")) j.at("record_enabled").get_to(v.record_enabled);
  if (j.contains("topic_meta_list")) j.at("topic_meta_list").get_to(v.topic_meta_list);
}

// --- GetRecordActionStatusReq ---

struct GetRecordActionStatusReq {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.GetRecordActionStatusReq";

  std::vector<std::string> action_names;  // if empty, then get all record actions
};

inline void to_json(nlohmann::json& j, const GetRecordActionStatusReq& v) {
  j = nlohmann::json{{"action_names", v.action_names}};
}

inline void from_json(const nlohmann::json& j, GetRecordActionStatusReq& v) {
  if (j.contains("action_names")) j.at("action_names").get_to(v.action_names);
}

// --- GetRecordActionStatusRsp ---

struct GetRecordActionStatusRsp {
  static constexpr const char* kTypeName =
      "ibmw.protocols.record_playback_extension.GetRecordActionStatusRsp";

  CommonRsp common_rsp;
  std::vector<RecordActionStatus> record_action_status_list;
};

inline void to_json(nlohmann::json& j, const GetRecordActionStatusRsp& v) {
  j = nlohmann::json{
      {"common_rsp", v.common_rsp},
      {"record_action_status_list", v.record_action_status_list}};
}

inline void from_json(const nlohmann::json& j, GetRecordActionStatusRsp& v) {
  if (j.contains("common_rsp")) j.at("common_rsp").get_to(v.common_rsp);
  if (j.contains("record_action_status_list")) j.at("record_action_status_list").get_to(v.record_action_status_list);
}

}  // namespace ibmw::protocols::record_playback_extension
