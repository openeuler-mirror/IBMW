// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <utility>

#include "engine/service/service_message_frame.h"

namespace ibmw::runtime::core::service {

inline std::shared_ptr<ibmw::util::BufferArrayView> SerializeReqWithCache(
    ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type) {
  const auto& info = invoke_wrapper.info;
  auto& req_serialization_cache = invoke_wrapper.req_serialization_cache;

  auto find_itr = req_serialization_cache.find(serialization_type);
  if (find_itr != req_serialization_cache.end())
    return find_itr->second;

  auto buffer_array_ptr = std::make_unique<ibmw::util::BufferArray>();
  bool serialize_ret = info.req_type_support_ref.Serialize(
      serialization_type,
      invoke_wrapper.req_ptr,
      buffer_array_ptr->AllocatorNativeHandle(),
      buffer_array_ptr->BufferArrayNativeHandle());

  MW_ASSERT(serialize_ret, "Serialize failed.");

  auto* ptr = buffer_array_ptr.get();
  auto buffer_array_view_ptr = std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(*ptr),
      [buffer_array_ptr{std::move(buffer_array_ptr)}](const auto* ptr) { delete ptr; });

  req_serialization_cache.emplace(serialization_type, buffer_array_view_ptr);

  return buffer_array_view_ptr;
}

inline std::shared_ptr<ibmw::util::BufferArrayView> TrySerializeReqWithCache(
    ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type) noexcept {
  try {
    return SerializeReqWithCache(invoke_wrapper, serialization_type);
  } catch (...) {
    return {};
  }
}

inline std::shared_ptr<ibmw::util::BufferArrayView> SerializeRspWithCache(
    ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type) {
  const auto& info = invoke_wrapper.info;
  auto& rsp_serialization_cache = invoke_wrapper.rsp_serialization_cache;

  auto find_itr = rsp_serialization_cache.find(serialization_type);
  if (find_itr != rsp_serialization_cache.end())
    return find_itr->second;

  auto buffer_array_ptr = std::make_unique<ibmw::util::BufferArray>();
  bool serialize_ret = info.rsp_type_support_ref.Serialize(
      serialization_type,
      invoke_wrapper.rsp_ptr,
      buffer_array_ptr->AllocatorNativeHandle(),
      buffer_array_ptr->BufferArrayNativeHandle());

  MW_ASSERT(serialize_ret, "Serialize failed.");

  auto* ptr = buffer_array_ptr.get();
  auto buffer_array_view_ptr = std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(*ptr),
      [buffer_array_ptr{std::move(buffer_array_ptr)}](const auto* ptr) { delete ptr; });

  rsp_serialization_cache.emplace(serialization_type, buffer_array_view_ptr);

  return buffer_array_view_ptr;
}

inline std::shared_ptr<ibmw::util::BufferArrayView> TrySerializeRspWithCache(
    ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type) noexcept {
  try {
    return SerializeRspWithCache(invoke_wrapper, serialization_type);
  } catch (...) {
    return {};
  }
}

inline void CallResultCallback(ServiceMessageFrame& invoke_wrapper, ibmw::rpc::Status status) {
  auto callback = std::move(invoke_wrapper.callback);
  invoke_wrapper.callback = nullptr;
  if (callback) {
    callback(status);
  }
}

}  // namespace ibmw::runtime::core::service
