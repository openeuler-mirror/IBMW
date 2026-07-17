// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/transport_catalog.h"

namespace ibmw::runtime::core::transport {

bool TransportCatalog::RegisterSendType(
    std::unique_ptr<SendTypeWrapper>&& send_type_wrapper_ptr) {
  const auto& info = send_type_wrapper_ptr->info;

  Key key{
      .msg_type = info.msg_type,
      .topic_name = info.topic_name,
      .pkg_path = info.pkg_path,
      .module_name = info.module_name};

  auto emplace_ret = send_type_wrapper_map_.try_emplace(
      key, std::move(send_type_wrapper_ptr));

  if (!emplace_ret.second) [[unlikely]] {
    MW_WARN(
        "Publish msg type '{}' is registered repeatedly, topic '{}', module '{}', pkg path '{}'",
        key.msg_type, key.topic_name, key.module_name, key.pkg_path);
    return false;
  }

  pub_topic_index_map_[key.topic_name].emplace_back(emplace_ret.first->second.get());

  MW_TRACE(
      "Publish msg type '{}' is successfully registered, topic '{}', module '{}', pkg path '{}'",
      key.msg_type, key.topic_name, key.module_name, key.pkg_path);

  return true;
}

bool TransportCatalog::OnMessage(
    std::unique_ptr<MessageHandlerWrapper>&& message_handler_ptr) {
  const auto& info = message_handler_ptr->info;

  Key key{
      .msg_type = info.msg_type,
      .topic_name = info.topic_name,
      .pkg_path = info.pkg_path,
      .module_name = info.module_name};

  auto emplace_ret = message_handler_map_.try_emplace(
      key, std::move(message_handler_ptr));

  if (!emplace_ret.second) [[unlikely]] {
    MW_WARN(
        "Msg type '{}' is subscribed repeatedly, topic '{}', module '{}', pkg path '{}'",
        key.msg_type, key.topic_name, key.module_name, key.pkg_path);
    return false;
  }

  sub_topic_index_map_[key.topic_name].emplace_back(emplace_ret.first->second.get());

  MTPKey m_t_p_key{
      .msg_type = key.msg_type,
      .topic_name = key.topic_name,
      .pkg_path = key.pkg_path};

  sub_msg_topic_pkg_index_map_[m_t_p_key][key.module_name] = emplace_ret.first->second.get();

  MW_TRACE(
      "Msg type '{}' is successfully subscribed, topic '{}', module '{}', pkg path '{}'",
      key.msg_type, key.topic_name, key.module_name, key.pkg_path);

  return true;
}

const MessageHandlerWrapper* TransportCatalog::GetMessageHandlerWrapperPtr(
    std::string_view msg_type,
    std::string_view topic_name,
    std::string_view pkg_path,
    std::string_view module_name) const {
  auto find_itr = message_handler_map_.find(
      Key{.msg_type = msg_type, .topic_name = topic_name, .pkg_path = pkg_path, .module_name = module_name});

  if (find_itr != message_handler_map_.end())
    return find_itr->second.get();

  return nullptr;
}

const TransportCatalog::ModuleMessageHandlerWrapperMap* TransportCatalog::GetModuleMessageHandlerWrapperMapPtr(
    std::string_view msg_type,
    std::string_view topic_name,
    std::string_view pkg_path) const {
  auto find_itr = sub_msg_topic_pkg_index_map_.find(
      MTPKey{.msg_type = msg_type, .topic_name = topic_name, .pkg_path = pkg_path});

  if (find_itr != sub_msg_topic_pkg_index_map_.end())
    return &(find_itr->second);

  return nullptr;
}

const SendTypeWrapper* TransportCatalog::GetSendTypeWrapperPtr(
    std::string_view msg_type,
    std::string_view topic_name,
    std::string_view pkg_path,
    std::string_view module_name) const {
  auto find_itr = send_type_wrapper_map_.find(
      Key{.msg_type = msg_type, .topic_name = topic_name, .pkg_path = pkg_path, .module_name = module_name});

  if (find_itr != send_type_wrapper_map_.end())
    return find_itr->second.get();

  return nullptr;
}

}  // namespace ibmw::runtime::core::transport
