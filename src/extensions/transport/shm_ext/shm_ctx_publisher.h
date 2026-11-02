// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Zero-copy ctx-meta publisher (counterpart of iceoryx_ext's
// IoxCtxMetaTypedPublisher): the message is serialized directly into a loaned
// shm chunk behind the ctx-meta header, then published. The wire layout is
// the one Send() produces, so the receive side is the standard OnMessage<T>
// stack.
//
// Only TransportDriver virtuals are used (LoanSerializedSample +
// PublishLoaned), so the driver is passed as the base pointer: look it up by
// name in the TransportBus, no dynamic_cast to ShmTransportDriver needed.
//
// Pre-condition: RegisterSendType for (topic, msg_type) happened in Init, so
// the driver has a publisher for the topic.
//
//   auto pub = CreateShmCtxMetaTypedPublisher<MyMsg>(driver, "topic", "dds:MyMsg");
//   ibmw::transport::Context ctx;
//   ctx.SetSerializationType("cdr");
//   pub->Publish(ctx, msg);

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "cpp/context/details/type_support.h"
#include "cpp/transport/transport_context.h"
#include "cpp/util/type_support.h"
#include "engine/transport/loaned_sample_handle.h"
#include "engine/transport/transport_driver.h"

namespace ibmw::extensions::shm_extension {

class ShmCtxMetaPublisher {
 public:
  ShmCtxMetaPublisher(runtime::core::transport::TransportDriver* driver, std::string topic, std::string msg_type,
                      ibmw::util::TypeSupportRef type_support)
      : driver_(driver) {
    info_.topic_name = std::move(topic);
    info_.msg_type = std::move(msg_type);
    info_.msg_type_support_ref = type_support;
  }

  // Loan, serialize `msg` into the chunk and publish. False if the loan or
  // the serialization failed; the driver then holds no chunk. True does not
  // prove delivery: TransportDriver::PublishLoaned() returns nothing, so a
  // publish the library refuses (the chunk is still released) reads as true.
  // Callers needing that result loan and call
  // ShmTransportDriver::TryPublishLoaned() themselves.
  bool Publish(const ibmw::transport::Context& ctx, const void* msg) noexcept {
    if (driver_ == nullptr || msg == nullptr) return false;
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSerializedSample(info_, ctx, msg, handle)) return false;
    driver_->PublishLoaned(handle);
    return true;
  }

  const runtime::core::transport::TransportInfo& info() const noexcept { return info_; }

 private:
  runtime::core::transport::TransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

template <typename MsgType>
class ShmCtxMetaTypedPublisher {
 public:
  ShmCtxMetaTypedPublisher(runtime::core::transport::TransportDriver* driver, std::string topic, std::string msg_type)
      : impl_(driver, std::move(topic), std::move(msg_type),
              ibmw::util::TypeSupportRef(ibmw::GetMessageTypeSupport<MsgType>())) {}

  bool Publish(const ibmw::transport::Context& ctx, const MsgType& msg) noexcept {
    return impl_.Publish(ctx, static_cast<const void*>(&msg));
  }

  const runtime::core::transport::TransportInfo& info() const noexcept { return impl_.info(); }

 private:
  ShmCtxMetaPublisher impl_;
};

// Returns nullptr if `driver` is null.
template <typename MsgType>
std::unique_ptr<ShmCtxMetaTypedPublisher<MsgType>> CreateShmCtxMetaTypedPublisher(
    runtime::core::transport::TransportDriver* driver, std::string_view topic, std::string_view msg_type) {
  if (driver == nullptr) return nullptr;
  return std::make_unique<ShmCtxMetaTypedPublisher<MsgType>>(driver, std::string(topic), std::string(msg_type));
}

}  // namespace ibmw::extensions::shm_extension
