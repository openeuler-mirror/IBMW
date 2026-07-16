// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// INTERNAL: alias-forwarding only. Not public API. Scheduled for removal
// after the compatibility window (see docs/reference/support-matrix.md).

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>

#include "cpp/transport/loaned_sample.h"
#include "cpp/transport/transport_handle.h"
#include "dds_types/util/dds_type_support.h"

namespace ibmw::transport {

/**
 * @brief Register native DDS message type for publishing
 *
 * @tparam DdsType The DDS message type
 * @param publisher The publisher reference
 * @param type_name Optional explicit DDS type name. If empty, uses DdsTypeName<DdsType>.
 * @return true if registration succeeded
 */
template <typename DdsType>
inline bool RegisterSendType(SenderRef publisher,
                                const std::string& type_name = "") {
  return publisher.RegisterSendType(
      ibmw::dds::GetDdsMessageTypeSupport<DdsType>(type_name));
}

/**
 * @brief Publish a native DDS message with context
 *
 * @tparam DdsType The DDS message type
 * @param publisher The publisher reference
 * @param ctx_ref The context reference
 * @param msg The DDS message to publish
 * @param type_name Optional explicit DDS type name. If empty, uses DdsTypeName<DdsType>.
 */
template <typename DdsType>
inline void Send(SenderRef publisher, ContextRef ctx_ref,
                    const DdsType& msg, const std::string& type_name = "") {
  static const std::string kMsgTypeName = [&]() {
    std::string name = type_name.empty()
                           ? ibmw::dds::DdsTypeName<DdsType>::get()
                           : type_name;
    return "dds:" + name;
  }();

  if (ctx_ref) {
    ctx_ref.SetSerializationType("cdr");
    publisher.Send(kMsgTypeName, ctx_ref, static_cast<const void*>(&msg));
    return;
  }

  publisher.Send(kMsgTypeName, ContextRef(), static_cast<const void*>(&msg));
}

/**
 * @brief Publish a native DDS message without context
 *
 * @tparam DdsType The DDS message type
 * @param publisher The publisher reference
 * @param msg The DDS message to publish
 * @param type_name Optional explicit DDS type name. If empty, uses DdsTypeName<DdsType>.
 */
template <typename DdsType>
inline void Send(SenderRef publisher, const DdsType& msg,
                    const std::string& type_name = "") {
  Send<DdsType>(publisher, ContextRef(), msg, type_name);
}

/**
 * @brief Subscribe to native DDS messages with context
 *
 * @tparam DdsType The DDS message type
 * @param subscriber The subscriber reference
 * @param callback The callback function receiving context and message
 * @param type_name Optional explicit DDS type name. If empty, uses DdsTypeName<DdsType>.
 * @return true if subscription succeeded
 */
template <typename DdsType>
inline bool OnMessage(
    ReceiverRef subscriber,
    std::function<void(ContextRef, const std::shared_ptr<const DdsType>&)>&& callback,
    const std::string& type_name = "") {
  return subscriber.OnMessage(
      ibmw::dds::GetDdsMessageTypeSupport<DdsType>(type_name),
      [callback{std::move(callback)}](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) {
        SubscriberReleaseCallback release_callback(release_callback_base);
        callback(ContextRef(ctx_ptr),
                 std::shared_ptr<const DdsType>(
                     static_cast<const DdsType*>(msg_ptr),
                     [release_callback{std::move(release_callback)}](const DdsType*) {
                       release_callback();
                     }));
      });
}

/**
 * @brief Subscribe to native DDS messages without context
 *
 * @tparam DdsType The DDS message type
 * @param subscriber The subscriber reference
 * @param callback The callback function receiving only the message
 * @param type_name Optional explicit DDS type name. If empty, uses DdsTypeName<DdsType>.
 * @return true if subscription succeeded
 */
template <typename DdsType>
inline bool OnMessage(
    ReceiverRef subscriber,
    std::function<void(const std::shared_ptr<const DdsType>&)>&& callback,
    const std::string& type_name = "") {
  return subscriber.OnMessage(
      ibmw::dds::GetDdsMessageTypeSupport<DdsType>(type_name),
      [callback{std::move(callback)}](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) {
        SubscriberReleaseCallback release_callback(release_callback_base);
        callback(std::shared_ptr<const DdsType>(
            static_cast<const DdsType*>(msg_ptr),
            [release_callback{std::move(release_callback)}](const DdsType*) {
              release_callback();
            }));
      });
}

}  // namespace ibmw::transport

namespace ibmw::dds {

/**
 * @brief Concept satisfied by any DDS type that has a DdsTypeName specialization
 *        AND supports FastCDR serialization (i.e., eprosima::fastcdr::serialize).
 *
 * In practice, all types registered with IBMW_DDS_TYPE_NAME() and generated
 * by fastddsgen satisfy this concept.
 */
template <typename T>
concept DdsMessage = requires {
  { ibmw::dds::DdsTypeName<T>::get() } -> std::convertible_to<std::string>;
};

}  // namespace ibmw::dds

namespace ibmw::transport {

template <ibmw::dds::DdsMessage MsgType>
class SenderAccess<MsgType> : public SenderAccessBase {
 public:
  explicit SenderAccess(SenderRef publisher)
      : SenderAccessBase(
            publisher,
            std::string("dds:") + ibmw::dds::DdsTypeName<MsgType>::get()) {}
  ~SenderAccess() = default;

  static bool RegisterSendType(SenderRef publisher) {
    return ibmw::transport::RegisterSendType<MsgType>(publisher);
  }

  bool RegisterSendType() {
    return publisher_.RegisterSendType(
        ibmw::dds::GetDdsMessageTypeSupport<MsgType>());
  }

  void Send(const MsgType& msg) {
    Send(ContextRef(), msg);
  }

  void Send(ContextRef ctx_ref, const MsgType& msg) {
    if (ctx_ref) {
      if (ctx_ref.GetSerializationType().empty()) ctx_ref.SetSerializationType("cdr");
      SendImpl(ctx_ref, static_cast<const void*>(&msg));
      return;
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetSerializationType("cdr");
    SendImpl(*ctx_ptr, static_cast<const void*>(&msg));
  }

  LoanedSample<MsgType> Loan(LoanMode mode = kLoanModePodOnly) {
    auto handle = LoanImpl(sizeof(MsgType), mode);
    if (handle.user_payload_ptr == nullptr) return {};
    return LoanedSample<MsgType>(GetSenderRef(), handle);
  }

  LoanedSample<MsgType> BorrowBytes(size_t bytes,
                                    LoanMode mode = kLoanModePodOnly) {
    if (bytes < sizeof(MsgType)) return {};
    auto handle = LoanImpl(bytes, mode);
    if (handle.user_payload_ptr == nullptr) return {};
    return LoanedSample<MsgType>(GetSenderRef(), handle);
  }

  bool Publish(LoanedSample<MsgType>&& sample) {
    if (!sample.is_valid()) return false;
    auto handle = sample.ExtractForPublish();
    PublishLoanedImpl(handle);
    return true;
  }
};

template <ibmw::dds::DdsMessage MsgType>
class ReceiverAccess<MsgType> : public ReceiverAccessBase {
 public:
  explicit ReceiverAccess(ReceiverRef subscriber)
      : ReceiverAccessBase(
            subscriber,
            std::string("dds:") + ibmw::dds::DdsTypeName<MsgType>::get()) {}
  ~ReceiverAccess() = default;

  bool OnMessage(
      std::function<void(ContextRef, const std::shared_ptr<const MsgType>&)>&& callback) {
    return ibmw::transport::OnMessage<MsgType>(subscriber_, std::move(callback));
  }

  bool OnMessage(
      std::function<void(const std::shared_ptr<const MsgType>&)>&& callback) {
    return ibmw::transport::OnMessage<MsgType>(subscriber_, std::move(callback));
  }
};

}  // namespace ibmw::transport
