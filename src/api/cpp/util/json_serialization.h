// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>

#include "cpp/transport/transport_handle.h"
#include "cpp/util/json_type_support.h"

namespace ibmw::json {

template <typename T>
concept JsonMessage = requires {
  { T::kTypeName } -> std::convertible_to<const char*>;
};

}  // namespace ibmw::json

namespace ibmw::json::transport {

/**
 * @brief Register a JSON message type for publishing.
 *
 * @tparam JsonType A struct with `static constexpr const char* kTypeName`
 *                  and nlohmann::json ADL support (to_json / from_json).
 * @param publisher The publisher reference.
 * @return true if registration succeeded.
 */
template <JsonMessage JsonType>
inline bool RegisterSendType(ibmw::transport::SenderRef publisher) {
  return publisher.RegisterSendType(
      ibmw::json::GetJsonMessageTypeSupport<JsonType>());
}

/**
 * @brief Send a JSON message with an explicit context.
 */
template <JsonMessage JsonType>
inline void Send(ibmw::transport::SenderRef publisher,
                 ibmw::transport::ContextRef ctx_ref,
                 const JsonType& msg) {
  static const std::string kMsgTypeName =
      "json:" + std::string(JsonType::kTypeName);

  if (ctx_ref) {
    if (ctx_ref.GetSerializationType().empty()) {
      ctx_ref.SetSerializationType("json");
    }
    publisher.Send(kMsgTypeName, ctx_ref, static_cast<const void*>(&msg));
    return;
  }

  ibmw::transport::Context ctx;
  ctx.SetSerializationType("json");
  publisher.Send(kMsgTypeName, ctx, static_cast<const void*>(&msg));
}

/**
 * @brief Send a JSON message without an explicit context.
 */
template <JsonMessage JsonType>
inline void Send(ibmw::transport::SenderRef publisher, const JsonType& msg) {
  Send<JsonType>(publisher, ibmw::transport::ContextRef(), msg);
}

/**
 * @brief Subscribe to JSON messages (callback receives context + shared_ptr).
 *
 * The release callback is automatically wrapped into the shared_ptr deleter,
 * so the user never needs to handle raw release callbacks.
 */
template <JsonMessage JsonType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    std::function<void(ibmw::transport::ContextRef,
                       const std::shared_ptr<const JsonType>&)>&& callback) {
  return subscriber.OnMessage(
      ibmw::json::GetJsonMessageTypeSupport<JsonType>(),
      [callback{std::move(callback)}](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) {
        ibmw::transport::SubscriberReleaseCallback release_callback(
            release_callback_base);
        callback(
            ibmw::transport::ContextRef(ctx_ptr),
            std::shared_ptr<const JsonType>(
                static_cast<const JsonType*>(msg_ptr),
                [release_callback{std::move(release_callback)}](const JsonType*) {
                  release_callback();
                }));
      });
}

/**
 * @brief Subscribe to JSON messages (callback receives shared_ptr only).
 */
template <JsonMessage JsonType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    std::function<void(const std::shared_ptr<const JsonType>&)>&& callback) {
  return OnMessage<JsonType>(
      subscriber,
      [callback{std::move(callback)}](
          ibmw::transport::ContextRef,
          const std::shared_ptr<const JsonType>& msg) { callback(msg); });
}

}  // namespace ibmw::json::transport

namespace ibmw::transport {

using ibmw::json::transport::OnMessage;
using ibmw::json::transport::RegisterSendType;
using ibmw::json::transport::Send;

}  // namespace ibmw::transport
