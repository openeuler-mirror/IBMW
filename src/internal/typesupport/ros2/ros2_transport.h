// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// INTERNAL: alias-forwarding only. Not public API. Scheduled for removal
// after the compatibility window (see docs/reference/support-matrix.md).

#pragma once

#include <memory>
#include <functional>
#include <string>
#include <type_traits>

#include <rosidl_runtime_cpp/traits.hpp>

#include "cpp/transport/loaned_sample.h"
#include "cpp/transport/transport_handle.h"
#include "ros2_types/util/ros2_type_support_factory.h"

namespace ibmw::ros2::transport {

/**
 * @brief Get the ROS2 type name string for a message type.
 *
 * Uses rosidl_generator_traits::name<T>() which every rosidl-generated
 * message type specializes (e.g. "example_ros2/msg/RosTestMsg").
 */
template <typename ROS2MsgType>
inline std::string GetRos2TypeName() {
  return rosidl_generator_traits::name<ROS2MsgType>();
}

/**
 * @brief Register a ROS2 message type for publishing (2-arg: explicit name)
 */
template <typename ROS2MsgType>
inline bool RegisterSendType(
    ibmw::transport::SenderRef publisher,
    const std::string& ros2_type_name) {
  auto* ts = GetIbmwTypeSupport<ROS2MsgType>(ros2_type_name);
  if (!ts) {
    return false;
  }
  return publisher.RegisterSendType(ts);
}

/**
 * @brief Register a ROS2 message type for publishing (1-arg: auto name)
 */
template <typename ROS2MsgType>
inline bool RegisterSendType(
    ibmw::transport::SenderRef publisher) {
  return RegisterSendType<ROS2MsgType>(publisher, GetRos2TypeName<ROS2MsgType>());
}

/**
 * @brief Send a ROS2 message (3-arg: explicit name)
 */
template <typename ROS2MsgType>
inline void Send(
    ibmw::transport::SenderRef publisher,
    const std::string& ros2_type_name,
    const ROS2MsgType& msg) {
  std::string ibmw_type_name = "ros2:" + ros2_type_name;

  ibmw::transport::Context ctx;
  ctx.SetSerializationType("ros2");

  publisher.Send(ibmw_type_name, ctx, static_cast<const void*>(&msg));
}

/**
 * @brief Send a ROS2 message (2-arg: auto name)
 */
template <typename ROS2MsgType>
inline void Send(
    ibmw::transport::SenderRef publisher,
    const ROS2MsgType& msg) {
  Send<ROS2MsgType>(publisher, GetRos2TypeName<ROS2MsgType>(), msg);
}

/**
 * @brief Send a ROS2 message (with context, explicit name)
 */
template <typename ROS2MsgType>
inline void Send(
    ibmw::transport::SenderRef publisher,
    const std::string& ros2_type_name,
    ibmw::transport::ContextRef ctx,
    const ROS2MsgType& msg) {
  std::string ibmw_type_name = "ros2:" + ros2_type_name;
  ctx.SetSerializationType("ros2");

  publisher.Send(ibmw_type_name, ctx, static_cast<const void*>(&msg));
}

/**
 * @brief Send a ROS2 message (with context, auto name)
 */
template <typename ROS2MsgType>
inline void Send(
    ibmw::transport::SenderRef publisher,
    ibmw::transport::ContextRef ctx,
    const ROS2MsgType& msg) {
  Send<ROS2MsgType>(publisher, GetRos2TypeName<ROS2MsgType>(), ctx, msg);
}

/**
 * @brief OnMessage to ROS2 messages (with context, explicit name)
 */
template <typename ROS2MsgType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    const std::string& ros2_type_name,
    std::function<void(ibmw::transport::ContextRef, const std::shared_ptr<const ROS2MsgType>&)>&& callback) {
  auto* ts = GetIbmwTypeSupport<ROS2MsgType>(ros2_type_name);
  if (!ts) {
    return false;
  }

  return subscriber.OnMessage(
      ts,
      [callback{std::move(callback)}](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) {
        ibmw::transport::SubscriberReleaseCallback release_callback(release_callback_base);

        callback(
            ibmw::transport::ContextRef(ctx_ptr),
            std::shared_ptr<const ROS2MsgType>(
                static_cast<const ROS2MsgType*>(msg_ptr),
                [release_callback{std::move(release_callback)}](const ROS2MsgType*) mutable {
                  release_callback();
                }));
      });
}

/**
 * @brief OnMessage to ROS2 messages (without context, explicit name)
 */
template <typename ROS2MsgType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    const std::string& ros2_type_name,
    std::function<void(const std::shared_ptr<const ROS2MsgType>&)>&& callback) {
  auto* ts = GetIbmwTypeSupport<ROS2MsgType>(ros2_type_name);
  if (!ts) {
    return false;
  }

  return subscriber.OnMessage(
      ts,
      [callback{std::move(callback)}](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* msg_ptr,
          ibmw_function_base_t* release_callback_base) {
        ibmw::transport::SubscriberReleaseCallback release_callback(release_callback_base);

        callback(
            std::shared_ptr<const ROS2MsgType>(
                static_cast<const ROS2MsgType*>(msg_ptr),
                [release_callback{std::move(release_callback)}](const ROS2MsgType*) mutable {
                  release_callback();
                }));
      });
}

/**
 * @brief OnMessage to ROS2 messages (with context, auto name)
 */
template <typename ROS2MsgType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    std::function<void(ibmw::transport::ContextRef, const std::shared_ptr<const ROS2MsgType>&)>&& callback) {
  return OnMessage<ROS2MsgType>(subscriber, GetRos2TypeName<ROS2MsgType>(), std::move(callback));
}

/**
 * @brief OnMessage for ROS2 messages (without context, auto name)
 */
template <typename ROS2MsgType>
inline bool OnMessage(
    ibmw::transport::ReceiverRef subscriber,
    std::function<void(const std::shared_ptr<const ROS2MsgType>&)>&& callback) {
  return OnMessage<ROS2MsgType>(subscriber, GetRos2TypeName<ROS2MsgType>(), std::move(callback));
}

}  // namespace ibmw::ros2::transport

namespace ibmw::transport {
using ibmw::ros2::transport::RegisterSendType;
using ibmw::ros2::transport::Send;
using ibmw::ros2::transport::OnMessage;
}  // namespace ibmw::transport

namespace ibmw::ros2 {

template <typename T>
concept Ros2Message = rosidl_generator_traits::is_message<T>::value;

}  // namespace ibmw::ros2

namespace ibmw::transport {

template <ibmw::ros2::Ros2Message MsgType>
class SenderAccess<MsgType> : public SenderAccessBase {
 public:
  explicit SenderAccess(SenderRef publisher)
      : SenderAccessBase(
            publisher,
            std::string("ros2:") + ibmw::ros2::transport::GetRos2TypeName<MsgType>()) {}
  ~SenderAccess() = default;

  static bool RegisterSendType(SenderRef publisher) {
    return ibmw::ros2::transport::RegisterSendType<MsgType>(publisher);
  }

  bool RegisterSendType() {
    return publisher_.RegisterSendType(
        ibmw::ros2::GetIbmwTypeSupport<MsgType>(
            ibmw::ros2::transport::GetRos2TypeName<MsgType>()));
  }

  void Send(const MsgType& msg) {
    Send(ContextRef(), msg);
  }

  void Send(ContextRef ctx_ref, const MsgType& msg) {
    if (ctx_ref) {
      if (ctx_ref.GetSerializationType().empty()) ctx_ref.SetSerializationType("ros2");
      SendImpl(ctx_ref, static_cast<const void*>(&msg));
      return;
    }

    auto ctx_ptr = NewContextSharedPtr();
    ctx_ptr->SetSerializationType("ros2");
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

template <ibmw::ros2::Ros2Message MsgType>
class ReceiverAccess<MsgType> : public ReceiverAccessBase {
 public:
  explicit ReceiverAccess(ReceiverRef subscriber)
      : ReceiverAccessBase(
            subscriber,
            std::string("ros2:") + ibmw::ros2::transport::GetRos2TypeName<MsgType>()) {}
  ~ReceiverAccess() = default;

  bool OnMessage(
      std::function<void(ContextRef, const std::shared_ptr<const MsgType>&)>&& callback) {
    return ibmw::ros2::transport::OnMessage<MsgType>(subscriber_, std::move(callback));
  }

  bool OnMessage(
      std::function<void(const std::shared_ptr<const MsgType>&)>&& callback) {
    return ibmw::ros2::transport::OnMessage<MsgType>(subscriber_, std::move(callback));
  }
};

}  // namespace ibmw::transport
