// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "c/transport/transport_handle_base.h"
#include "cpp/util/string.h"
#include "engine/transport/loaned_sample_handle.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "base/log_util.h"
#include "base/string_util.h"

namespace ibmw::runtime::core::transport {

// Phase 7 §7.3 §K-3.5: ABI bridge for the loan fast lane.
//
// The C ABI exposes ibmw_loaned_sample_handle_t which is a POD mirror
// of LoanedSampleHandle. We pass handles across the C boundary by
// reinterpret_cast on a pointer rather than field-by-field copy, so the
// two struct layouts MUST match exactly. These static_asserts catch any
// drift at compile time.
static_assert(sizeof(ibmw_loaned_sample_handle_t) == sizeof(LoanedSampleHandle),
              "ibmw_loaned_sample_handle_t / LoanedSampleHandle size mismatch");
static_assert(alignof(ibmw_loaned_sample_handle_t) == alignof(LoanedSampleHandle),
              "ibmw_loaned_sample_handle_t / LoanedSampleHandle align mismatch");
static_assert(offsetof(ibmw_loaned_sample_handle_t, user_payload_ptr) ==
                  offsetof(LoanedSampleHandle, user_payload_ptr),
              "user_payload_ptr offset mismatch");
static_assert(offsetof(ibmw_loaned_sample_handle_t, user_payload_size) ==
                  offsetof(LoanedSampleHandle, user_payload_size),
              "user_payload_size offset mismatch");
static_assert(offsetof(ibmw_loaned_sample_handle_t, mode) ==
                  offsetof(LoanedSampleHandle, mode),
              "mode offset mismatch");
static_assert(offsetof(ibmw_loaned_sample_handle_t, driver_token) ==
                  offsetof(LoanedSampleHandle, driver_token),
              "driver_token offset mismatch");
static_assert(offsetof(ibmw_loaned_sample_handle_t, source_driver) ==
                  offsetof(LoanedSampleHandle, source_driver),
              "source_driver offset mismatch");
static_assert(static_cast<uint8_t>(LoanMode::kPodOnly) == IBMW_LOAN_MODE_POD_ONLY,
              "LoanMode::kPodOnly value drift");
static_assert(static_cast<uint8_t>(LoanMode::kCtxMetaThenPayload) ==
                  IBMW_LOAN_MODE_CTX_META_THEN_PAYLOAD,
              "LoanMode::kCtxMetaThenPayload value drift");

class SenderAccess {
 public:
  SenderAccess(std::string_view pkg_path,
                 std::string_view module_name,
                 std::string_view topic_name,
                 ibmw::common::util::LoggerWrapper& logger,
                 TransportDriverDispatcher& transport_driver_bus,
                 const std::unordered_set<std::string>& passed_context_meta_keys)
      : pkg_path_(pkg_path),
        module_name_(module_name),
        topic_name_(topic_name),
        logger_(logger),
        transport_driver_dispatcher_(transport_driver_bus),
        passed_context_meta_keys_(passed_context_meta_keys),
        base_(GenBase(this)) {}
  ~SenderAccess() = default;

  SenderAccess(const SenderAccess&) = delete;
  SenderAccess& operator=(const SenderAccess&) = delete;

  const ibmw_transport_sender_base_t* NativeHandle() const { return &base_; }

  const ibmw::common::util::LoggerWrapper& GetLogger() const { return logger_; }

 private:
  bool RegisterSendType(const ibmw_type_support_base_t* msg_type_support) {
    return transport_driver_dispatcher_.RegisterSendType(
        RegisterSendTypeAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .topic_name = topic_name_,
            .msg_type_support = msg_type_support});
  }

  void Send(ibmw_string_view_t msg_type,
               const ibmw_transport_context_base_t* ctx_ptr,
               const void* msg_ptr) {
    transport_driver_dispatcher_.Send(
        SendAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .topic_name = topic_name_,
            .msg_type = msg_type,
            .ctx_ptr = ctx_ptr,
            .msg_ptr = msg_ptr});
  }

  void MergeSubscribeContextToPublishContext(
      const ibmw_transport_context_base_t* subscribe_ctx_ptr, const ibmw_transport_context_base_t* publish_ctx_ptr) {
    ibmw::transport::ContextRef subscribe_ctx_ref(subscribe_ctx_ptr);
    ibmw::transport::ContextRef publish_ctx_ref(publish_ctx_ptr);

    if (subscribe_ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT ||
        publish_ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT) [[unlikely]] {
      MW_WARN("Invalid context type!");
      return;
    }

    for (const auto& key : passed_context_meta_keys_) {
      auto val = subscribe_ctx_ref.GetMetaValue(key);
      if (!val.empty()) publish_ctx_ref.SetMetaValue(key, val);
    }
  }

  // Phase 7 §7.3 §K-3.5: loan fast lane C++ shims. They translate the
  // C ABI calls into LoanAccessInfoWrapper / LoanedSampleHandle and
  // forward to the dispatcher. The handle is reinterpret_cast across
  // the boundary; static_asserts at the top of this header guarantee
  // layout equivalence.
  void Loan(ibmw_string_view_t msg_type,
            size_t requested_size,
            ibmw_loan_mode_t mode,
            ibmw_loaned_sample_handle_t* out_handle) {
    auto handle = transport_driver_dispatcher_.Loan(
        LoanAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .topic_name = topic_name_,
            .msg_type = msg_type,
            .requested_size = requested_size,
            .mode = static_cast<LoanMode>(mode)});
    *reinterpret_cast<LoanedSampleHandle*>(out_handle) = handle;
  }

  void PublishLoaned(ibmw_loaned_sample_handle_t* handle) {
    transport_driver_dispatcher_.PublishLoaned(
        *reinterpret_cast<LoanedSampleHandle*>(handle));
  }

  void ReleaseLoaned(ibmw_loaned_sample_handle_t* handle) {
    transport_driver_dispatcher_.ReleaseLoaned(
        *reinterpret_cast<LoanedSampleHandle*>(handle));
  }

  static ibmw_transport_sender_base_t GenBase(void* ctx) {
    return ibmw_transport_sender_base_t{
        .register_send_type = [](void* ctx, const ibmw_type_support_base_t* msg_type_support) -> bool {
          return static_cast<SenderAccess*>(ctx)->RegisterSendType(msg_type_support);
        },
        .send = [](void* ctx,
                      ibmw_string_view_t msg_type,
                      const ibmw_transport_context_base_t* ctx_ptr,
                      const void* msg_ptr) {
          static_cast<SenderAccess*>(ctx)->Send(msg_type, ctx_ptr, msg_ptr);  //
        },
        .get_topic = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(static_cast<SenderAccess*>(ctx)->topic_name_);
        },
        .merge_subscribe_context_to_publish_context = [](void* ctx,                                             //
                                                         const ibmw_transport_context_base_t* subscribe_ctx_ptr,  //
                                                         const ibmw_transport_context_base_t* publish_ctx_ptr) {  //
          static_cast<SenderAccess*>(ctx)->MergeSubscribeContextToPublishContext(subscribe_ctx_ptr, publish_ctx_ptr);
        },
        .loan = [](void* ctx,
                   ibmw_string_view_t msg_type,
                   size_t requested_size,
                   ibmw_loan_mode_t mode,
                   ibmw_loaned_sample_handle_t* out_handle) {
          static_cast<SenderAccess*>(ctx)->Loan(msg_type, requested_size, mode, out_handle);
        },
        .publish_loaned = [](void* ctx, ibmw_loaned_sample_handle_t* handle) {
          static_cast<SenderAccess*>(ctx)->PublishLoaned(handle);
        },
        .release_loaned = [](void* ctx, ibmw_loaned_sample_handle_t* handle) {
          static_cast<SenderAccess*>(ctx)->ReleaseLoaned(handle);
        },
        .ctx = ctx};
  }

 private:
  const std::string pkg_path_;
  const std::string module_name_;
  const std::string topic_name_;

  ibmw::common::util::LoggerWrapper& logger_;

  TransportDriverDispatcher& transport_driver_dispatcher_;

  const std::unordered_set<std::string>& passed_context_meta_keys_;

  const ibmw_transport_sender_base_t base_;
};

class ReceiverAccess {
 public:
  ReceiverAccess(std::string_view pkg_path,
                  std::string_view module_name,
                  std::string_view topic_name,
                  TransportDriverDispatcher& transport_driver_bus)
      : pkg_path_(pkg_path),
        module_name_(module_name),
        topic_name_(topic_name),
        transport_driver_dispatcher_(transport_driver_bus),
        base_(GenBase(this)) {}
  ~ReceiverAccess() = default;

  ReceiverAccess(const ReceiverAccess&) = delete;
  ReceiverAccess& operator=(const ReceiverAccess&) = delete;

  const ibmw_transport_receiver_base_t* NativeHandle() const { return &base_; }

 private:
  bool OnMessage(const ibmw_type_support_base_t* msg_type_support,
                 ibmw_function_base_t* callback) {
    return transport_driver_dispatcher_.OnMessage(
        MessageHandlerAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .topic_name = topic_name_,
            .msg_type_support = msg_type_support,
            .callback = callback});
  }

  // Phase 7 §7.3 §U: receiver-side zero-copy fast lane. Mirrors
  // SenderAccess loan dispatch; iterates drivers first-success-wins via
  // the dispatcher and returns false if no driver supports zero-copy
  // receive for this topic+type.
  bool AttachLoaned(const ibmw_type_support_base_t* msg_type_support,
                    ibmw_function_base_t* callback) {
    return transport_driver_dispatcher_.AttachLoaned(
        MessageHandlerAccessInfoWrapper{
            .pkg_path = pkg_path_,
            .module_name = module_name_,
            .topic_name = topic_name_,
            .msg_type_support = msg_type_support,
            .callback = callback});
  }

  static ibmw_transport_receiver_base_t GenBase(void* ctx) {
    return ibmw_transport_receiver_base_t{
        .on_message = [](void* ctx,
                        const ibmw_type_support_base_t* msg_type_support,
                        ibmw_function_base_t* callback) -> bool {
          return static_cast<ReceiverAccess*>(ctx)->OnMessage(msg_type_support, callback);
        },
        .attach_loaned = [](void* ctx,
                           const ibmw_type_support_base_t* msg_type_support,
                           ibmw_function_base_t* callback) -> bool {
          return static_cast<ReceiverAccess*>(ctx)->AttachLoaned(msg_type_support, callback);
        },
        .get_topic = [](void* ctx) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(static_cast<ReceiverAccess*>(ctx)->topic_name_);
        },
        .ctx = ctx};
  }

 private:
  const std::string pkg_path_;
  const std::string module_name_;
  const std::string topic_name_;

  TransportDriverDispatcher& transport_driver_dispatcher_;

  const ibmw_transport_receiver_base_t base_;
};

class TransportAccess {
 public:
  using SenderAccessMap = std::unordered_map<
      std::string,
      std::unique_ptr<SenderAccess>,
      ibmw::common::util::StringHash,
      std::equal_to<>>;

  using ReceiverAccessMap = std::unordered_map<
      std::string,
      std::unique_ptr<ReceiverAccess>,
      ibmw::common::util::StringHash,
      std::equal_to<>>;

  TransportAccess(std::string_view pkg_path,
                     std::string_view module_name,
                     ibmw::common::util::LoggerWrapper& logger,
                     TransportDriverDispatcher& transport_driver_bus,
                     const std::unordered_set<std::string>& passed_context_meta_keys,
                     std::atomic_bool& start_flag,
                     SenderAccessMap& publisher_access_map,
                     ReceiverAccessMap& subscriber_access_map)
      : pkg_path_(pkg_path),
        module_name_(module_name),
        logger_(logger),
        transport_driver_dispatcher_(transport_driver_bus),
        passed_context_meta_keys_(passed_context_meta_keys),
        start_flag_(start_flag),
        publisher_access_map_(publisher_access_map),
        subscriber_access_map_(subscriber_access_map),
        base_(GenBase(this)) {}

  ~TransportAccess() = default;

  TransportAccess(const TransportAccess&) = delete;
  TransportAccess& operator=(const TransportAccess&) = delete;

  const ibmw_transport_handle_base_t* NativeHandle() const { return &base_; }

  const ibmw::common::util::LoggerWrapper& GetLogger() const { return logger_; }

 private:
  const ibmw_transport_sender_base_t* GetPublisher(ibmw_string_view_t input_topic) {
    auto topic = ibmw::util::ToStdStringView(input_topic);

    auto find_itr = publisher_access_map_.find(topic);
    if (find_itr != publisher_access_map_.end()) {
      return find_itr->second->NativeHandle();
    }

    if (start_flag_.load()) {
      MW_WARN("Can not get new publisher for topic '{}' after start.", topic);
      return nullptr;
    }

    auto emplace_ret = publisher_access_map_.emplace(
        topic,
        std::make_unique<SenderAccess>(
            pkg_path_,
            module_name_,
            topic,
            logger_,
            transport_driver_dispatcher_,
            passed_context_meta_keys_));

    return emplace_ret.first->second->NativeHandle();
  }

  const ibmw_transport_receiver_base_t* GetSubscriber(ibmw_string_view_t input_topic) {
    auto topic = ibmw::util::ToStdStringView(input_topic);

    auto find_itr = subscriber_access_map_.find(topic);
    if (find_itr != subscriber_access_map_.end()) {
      return find_itr->second->NativeHandle();
    }

    if (start_flag_.load()) {
      MW_WARN("Can not get new subscriber for topic '{}' after start.", topic);
      return nullptr;
    }

    auto emplace_ret = subscriber_access_map_.emplace(
        topic,
        std::make_unique<ReceiverAccess>(
            pkg_path_,
            module_name_,
            topic,
            transport_driver_dispatcher_));

    return emplace_ret.first->second->NativeHandle();
  }

  void MergeSubscribeContextToPublishContext(
      const ibmw_transport_context_base_t* subscribe_ctx_ptr, const ibmw_transport_context_base_t* publish_ctx_ptr) {
    ibmw::transport::ContextRef subscribe_ctx_ref(subscribe_ctx_ptr);
    ibmw::transport::ContextRef publish_ctx_ref(publish_ctx_ptr);

    if (subscribe_ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT ||
        publish_ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT) [[unlikely]] {
      MW_WARN("Invalid context type!");
      return;
    }

    for (const auto& key : passed_context_meta_keys_) {
      auto val = subscribe_ctx_ref.GetMetaValue(key);
      if (!val.empty()) publish_ctx_ref.SetMetaValue(key, val);
    }
  }

  static ibmw_transport_handle_base_t GenBase(void* ctx) {
    return ibmw_transport_handle_base_t{
        .get_sender = [](void* ctx, ibmw_string_view_t topic) -> const ibmw_transport_sender_base_t* {
          return static_cast<TransportAccess*>(ctx)->GetPublisher(topic);
        },
        .get_receiver = [](void* ctx, ibmw_string_view_t topic) -> const ibmw_transport_receiver_base_t* {
          return static_cast<TransportAccess*>(ctx)->GetSubscriber(topic);
        },
        .merge_subscribe_context_to_publish_context = [](void* ctx,                                             //
                                                         const ibmw_transport_context_base_t* subscribe_ctx_ptr,  //
                                                         const ibmw_transport_context_base_t* publish_ctx_ptr) {  //
          static_cast<TransportAccess*>(ctx)->MergeSubscribeContextToPublishContext(subscribe_ctx_ptr, publish_ctx_ptr);
        },
        .ctx = ctx};
  }

 private:
  const std::string pkg_path_;
  const std::string module_name_;

  ibmw::common::util::LoggerWrapper& logger_;

  TransportDriverDispatcher& transport_driver_dispatcher_;

  const std::unordered_set<std::string>& passed_context_meta_keys_;

  std::atomic_bool& start_flag_;
  SenderAccessMap& publisher_access_map_;
  ReceiverAccessMap& subscriber_access_map_;

  const ibmw_transport_handle_base_t base_;
};

}  // namespace ibmw::runtime::core::transport
