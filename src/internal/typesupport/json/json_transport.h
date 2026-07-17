// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// INTERNAL: alias-forwarding only. Not public API. Scheduled for removal
// after the compatibility window (see docs/reference/support-matrix.md).

#pragma once

#include "cpp/transport/loaned_sample.h"
#include "cpp/util/json_serialization.h"

namespace ibmw::transport {

template <ibmw::json::JsonMessage MsgType>
class SenderAccess<MsgType> : public SenderAccessBase {
 public:
  explicit SenderAccess(SenderRef publisher)
      : SenderAccessBase(
            publisher,
            std::string("json:") + std::string(MsgType::kTypeName)) {}
  ~SenderAccess() = default;

  static bool RegisterSendType(SenderRef publisher) {
    return ibmw::json::transport::RegisterSendType<MsgType>(publisher);
  }

  bool RegisterSendType() {
    return publisher_.RegisterSendType(
        ibmw::json::GetJsonMessageTypeSupport<MsgType>());
  }

  void Send(const MsgType& msg) { Send(ContextRef(), msg); }

  void Send(ContextRef ctx_ref, const MsgType& msg) {
    if (ctx_ref) {
      if (ctx_ref.GetSerializationType().empty()) {
        ctx_ref.SetSerializationType("json");
      }
      SendImpl(ctx_ref, static_cast<const void*>(&msg));
      return;
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetSerializationType("json");
    SendImpl(*ctx_ptr, static_cast<const void*>(&msg));
  }

  LoanedSample<MsgType> Loan(LoanMode mode = kLoanModePodOnly) {
    auto handle = LoanImpl(sizeof(MsgType), mode);
    if (handle.user_payload_ptr == nullptr) {
      return {};
    }
    return LoanedSample<MsgType>(GetSenderRef(), handle);
  }

  LoanedSample<MsgType> BorrowBytes(size_t bytes,
                                    LoanMode mode = kLoanModePodOnly) {
    if (bytes < sizeof(MsgType)) {
      return {};
    }
    auto handle = LoanImpl(bytes, mode);
    if (handle.user_payload_ptr == nullptr) {
      return {};
    }
    return LoanedSample<MsgType>(GetSenderRef(), handle);
  }

  bool Publish(LoanedSample<MsgType>&& sample) {
    if (!sample.is_valid()) {
      return false;
    }
    auto handle = sample.ExtractForPublish();
    PublishLoanedImpl(handle);
    return true;
  }
};

template <ibmw::json::JsonMessage MsgType>
class ReceiverAccess<MsgType> : public ReceiverAccessBase {
 public:
  explicit ReceiverAccess(ReceiverRef subscriber)
      : ReceiverAccessBase(
            subscriber,
            std::string("json:") + std::string(MsgType::kTypeName)) {}
  ~ReceiverAccess() = default;

  bool OnMessage(
      std::function<void(ContextRef, const std::shared_ptr<const MsgType>&)>&&
          callback) {
    return ibmw::json::transport::OnMessage<MsgType>(subscriber_,
                                                     std::move(callback));
  }

  bool OnMessage(
      std::function<void(const std::shared_ptr<const MsgType>&)>&& callback) {
    return ibmw::json::transport::OnMessage<MsgType>(subscriber_,
                                                     std::move(callback));
  }
};

}  // namespace ibmw::transport
