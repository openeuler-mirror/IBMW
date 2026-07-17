// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <c/util/type_support_base.h>
#include <cpp/transport/transport_handle.h>
#include <cpp/co/task.h>
#include <cpp/context/details/concepts.h>
#include <cpp/util/function.h>

#include <cassert>
#include <memory>

namespace ibmw::context::details {

template <class T>
std::shared_ptr<const T> MakeSharedMessage(
    const void* msg_raw_ptr,
    ibmw_function_base_t* release_callback_base) {
  ibmw::transport::SubscriberReleaseCallback release_callback(release_callback_base);
  return std::shared_ptr<const T>(
      static_cast<const T*>(msg_raw_ptr),
      [release_callback = std::move(release_callback)](const T*) mutable { release_callback(); });
}
}  // namespace ibmw::context::details

namespace ibmw {

template <typename T>
struct MessageTypeSupportTraits {
  static const ibmw_type_support_base_t* Get() {
    static_assert(std::is_same_v<T, void>,
                  "GetMessageTypeSupport<T> is not specialized for this type T. "
                  "Please include the correct generated typesupport header for your message "
                  "(see your <module>_typesupport.inc.h or the appropriate ibmw_*_types.h).");
    return nullptr;
  }
};

template <class T>
const ibmw_type_support_base_t* GetMessageTypeSupport() {
  return MessageTypeSupportTraits<T>::Get();
}

}  // namespace ibmw

namespace ibmw::transport {

template <class T>
struct MessagePublisherTraits {
  static void PublishMsg(ibmw::transport::SenderRef publisher, ibmw::transport::ContextRef ctx_ref, const T& msg) {
    static_assert(std::is_same_v<T, void>,
                  "Publish<T> is not specialized for this message type. "
                  "Please include the correct generated typesupport header for your message "
                  "(see your <module>_typesupport.inc.h or the appropriate ibmw_*_types.h).");
  }
};

template <class T>
void PublishMsg(ibmw::transport::SenderRef publisher, ibmw::transport::ContextRef ctx_ref, const T& msg) {
  MessagePublisherTraits<T>::PublishMsg(publisher, ctx_ref, msg);
  return;
}

}  // namespace ibmw::transport
