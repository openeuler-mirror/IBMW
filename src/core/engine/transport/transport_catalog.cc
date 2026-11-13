// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/transport_catalog.h"

#include <algorithm>

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

void TransportCatalog::RetireSendType(const SendTypeWrapper& send_type_wrapper) {
  const auto& info = send_type_wrapper.info;
  auto find_itr = send_type_wrapper_map_.find(Key{
      .msg_type = info.msg_type,
      .topic_name = info.topic_name,
      .pkg_path = info.pkg_path,
      .module_name = info.module_name});
  if (find_itr == send_type_wrapper_map_.end() || find_itr->second.get() != &send_type_wrapper) return;

  // Keys of both maps view strings owned by the wrapper, which stays alive.
  auto topic_itr = pub_topic_index_map_.find(info.topic_name);
  if (topic_itr != pub_topic_index_map_.end()) {
    auto& wrappers = topic_itr->second;
    wrappers.erase(std::remove(wrappers.begin(), wrappers.end(), &send_type_wrapper), wrappers.end());
    if (wrappers.empty()) pub_topic_index_map_.erase(topic_itr);
  }
  retired_send_type_wrappers_.emplace_back(std::move(find_itr->second));
  send_type_wrapper_map_.erase(find_itr);
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

bool TransportCatalog::ContainsMessageHandler(
    std::string_view msg_type,
    std::string_view topic_name,
    std::string_view pkg_path,
    std::string_view module_name) const {
  return message_handler_map_.contains(
      Key{.msg_type = msg_type,
          .topic_name = topic_name,
          .pkg_path = pkg_path,
          .module_name = module_name});
}

bool TransportCatalog::EraseMessageHandler(
    std::string_view msg_type,
    std::string_view topic_name,
    std::string_view pkg_path,
    std::string_view module_name) {
  const Key key{.msg_type = msg_type,
                .topic_name = topic_name,
                .pkg_path = pkg_path,
                .module_name = module_name};
  const auto handler_itr = message_handler_map_.find(key);
  if (handler_itr == message_handler_map_.end()) {
    return false;
  }

  auto* handler = handler_itr->second.get();
  if (auto topic_itr = sub_topic_index_map_.find(topic_name);
      topic_itr != sub_topic_index_map_.end()) {
    auto& handlers = topic_itr->second;
    handlers.erase(std::remove(handlers.begin(), handlers.end(), handler),
                   handlers.end());
    if (handlers.empty()) {
      sub_topic_index_map_.erase(topic_itr);
    }
  }

  const MTPKey mtp_key{.msg_type = msg_type,
                       .topic_name = topic_name,
                       .pkg_path = pkg_path};
  if (auto mtp_itr = sub_msg_topic_pkg_index_map_.find(mtp_key);
      mtp_itr != sub_msg_topic_pkg_index_map_.end()) {
    mtp_itr->second.erase(module_name);
    if (mtp_itr->second.empty()) {
      sub_msg_topic_pkg_index_map_.erase(mtp_itr);
    }
  }

  // The index keys are string_views into the owned wrapper, so destroy the
  // wrapper only after all secondary indexes have been removed.
  message_handler_map_.erase(handler_itr);
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
