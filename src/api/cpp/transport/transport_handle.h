// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <string_view>
#include <utility>

#include "c/transport/transport_handle_base.h"
#include "cpp/transport/transport_context.h"
#include "cpp/util/function.h"
#include "cpp/util/string.h"
#include "base/exception.h"

namespace ibmw::transport {

// Phase 7 §7.3 §K-3.5: re-export the C ABI loan POD types under a
// friendlier C++ alias so application code (and the upcoming
// LoanedSample<T> RAII template, §3.2) does not have to reach into the
// `c/transport/...` headers directly.
using LoanedSampleHandle = ibmw_loaned_sample_handle_t;
using LoanMode = ibmw_loan_mode_t;
inline constexpr LoanMode kLoanModePodOnly = IBMW_LOAN_MODE_POD_ONLY;
inline constexpr LoanMode kLoanModeCtxMetaThenPayload = IBMW_LOAN_MODE_CTX_META_THEN_PAYLOAD;

using SubscriberReleaseCallback = ibmw::util::Function<ibmw_function_receiver_release_callback_ops_t>;
using MessageCallback = ibmw::util::Function<ibmw_function_receiver_callback_ops_t>;
// Phase 7 §7.3 §U: receiver-side counterpart of the publisher loan fast
// lane. Payload pointer + size delivered straight from the driver with
// no deserialization. Used by LoanedSubscriber<T> (loaned_subscriber.h).
using LoanedMessageCallback = ibmw::util::Function<ibmw_function_loaned_receiver_callback_ops_t>;

struct RetainedLoanOptions {
  // Samples arriving while this many loans are outstanding are dropped and
  // are not delivered later. Releasing a loan permits future deliveries.
  size_t max_outstanding = 1;
};

class ReceivedLoan {
 public:
  ReceivedLoan() = default;
  ~ReceivedLoan() { Release(); }

  ReceivedLoan(const ReceivedLoan&) = delete;
  ReceivedLoan& operator=(const ReceivedLoan&) = delete;

  ReceivedLoan(ReceivedLoan&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)),
        release_(std::move(other.release_)) {}

  ReceivedLoan& operator=(ReceivedLoan&& other) noexcept {
    if (this != &other) {
      Release();
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
      release_ = std::move(other.release_);
    }
    return *this;
  }

  const void* Data() const noexcept { return data_; }
  size_t Size() const noexcept { return size_; }
  const void* data() const noexcept { return data_; }
  size_t size() const noexcept { return size_; }
  explicit operator bool() const noexcept { return static_cast<bool>(release_); }

  void Release() noexcept {
    if (release_) {
      auto release = std::move(release_);
      release();
    }
    data_ = nullptr;
    size_ = 0;
  }

  ReceivedLoan(const void* data,
               size_t size,
               ibmw_function_base_t* release_callback) noexcept
      : data_(data), size_(size), release_(release_callback) {}

 private:
  const void* data_ = nullptr;
  size_t size_ = 0;
  SubscriberReleaseCallback release_;
};

class LoanedSubscription {
 public:
  LoanedSubscription() = default;
  // Requests stop and deregistration without waiting. Use Close() when
  // quiescence is required; callbacks and loans may keep driver state alive.
  ~LoanedSubscription() { Reset(); }

  LoanedSubscription(const LoanedSubscription&) = delete;
  LoanedSubscription& operator=(const LoanedSubscription&) = delete;

  LoanedSubscription(LoanedSubscription&& other) noexcept
      : base_ptr_(std::exchange(other.base_ptr_, nullptr)) {}

  LoanedSubscription& operator=(LoanedSubscription&& other) noexcept {
    if (this != &other) {
      Reset();
      base_ptr_ = std::exchange(other.base_ptr_, nullptr);
    }
    return *this;
  }

  explicit operator bool() const noexcept { return base_ptr_ != nullptr; }

  void Stop() noexcept {
    if (base_ptr_ != nullptr && base_ptr_->stop != nullptr) {
      base_ptr_->stop(base_ptr_->ctx);
    }
  }

  bool Close(std::chrono::milliseconds timeout) noexcept {
    if (base_ptr_ == nullptr) return true;
    if (base_ptr_->stop == nullptr || base_ptr_->close == nullptr) return false;
    Stop();
    return base_ptr_->close(
        base_ptr_->ctx, static_cast<uint64_t>(timeout.count() < 0 ? 0 : timeout.count()));
  }

  void Reset() noexcept {
    if (base_ptr_ != nullptr) {
      if (base_ptr_->destroy != nullptr) base_ptr_->destroy(base_ptr_->ctx);
      base_ptr_ = nullptr;
    }
  }

 private:
  friend class ReceiverRef;
  explicit LoanedSubscription(ibmw_loaned_subscription_base_t* base_ptr) noexcept
      : base_ptr_(base_ptr) {}

  ibmw_loaned_subscription_base_t* base_ptr_ = nullptr;
};

class SenderRef {
 public:
  SenderRef() = default;
  explicit SenderRef(const ibmw_transport_sender_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~SenderRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_transport_sender_base_t* NativeHandle() const {
    return base_ptr_;
  }

  /**
   * @brief Register a type to be published
   *
   * @param msg_type_support
   * @return Register result
   */
  bool RegisterSendType(const ibmw_type_support_base_t* msg_type_support) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->register_send_type(base_ptr_->ctx, msg_type_support);
  }

  bool RegisterLoanedPodTopic(std::string_view msg_type,
                              size_t initial_size_hint,
                              size_t alignment = alignof(std::max_align_t)) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (base_ptr_->register_loaned_pod_topic == nullptr) return false;
    return base_ptr_->register_loaned_pod_topic(
        base_ptr_->ctx, ibmw::util::ToIbmwStringView(msg_type),
        initial_size_hint, alignment);
  }

  /**
   * @brief Publish a msg
   *
   * @param msg_type
   * @param msg_ptr
   */
  void Send(std::string_view msg_type, ContextRef ctx_ref, const void* msg_ptr) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->send(base_ptr_->ctx, ibmw::util::ToIbmwStringView(msg_type), ctx_ref.NativeHandle(), msg_ptr);
  }

  /**
   * @brief Get the topic for current publisher
   *
   * @return Topic for current publisher
   */
  std::string_view GetTopic() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return ibmw::util::ToStdStringView(base_ptr_->get_topic(base_ptr_->ctx));
  }

  void MergeSubscribeContextToPublishContext(
      const ContextRef subscribe_ctx_ref, ContextRef publish_ctx_ref) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->merge_subscribe_context_to_publish_context(
        base_ptr_->ctx,
        subscribe_ctx_ref.NativeHandle(),
        publish_ctx_ref.NativeHandle());
  }

  /**
   * @brief Borrow a loaned sample for zero-copy publishing.
   *
   * Phase 7 §7.3 §K-3.5: user-level zero-copy fast lane. The returned
   * handle has user_payload_ptr == nullptr if no transport could
   * fulfill the loan; callers MUST check before writing. Pair every
   * successful Loan() with exactly one PublishLoaned() or
   * ReleaseLoaned() call (typically wrapped by LoanedSample<T> RAII).
   */
  LoanedSampleHandle Loan(std::string_view msg_type,
                          size_t requested_size,
                          LoanMode mode = kLoanModePodOnly) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    LoanedSampleHandle handle{};
    base_ptr_->loan(base_ptr_->ctx,
                    ibmw::util::ToIbmwStringView(msg_type),
                    requested_size,
                    mode,
                    &handle);
    return handle;
  }

  void PublishLoaned(LoanedSampleHandle& handle) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->publish_loaned(base_ptr_->ctx, &handle);
  }

  void ReleaseLoaned(LoanedSampleHandle& handle) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->release_loaned(base_ptr_->ctx, &handle);
  }

 private:
  const ibmw_transport_sender_base_t* base_ptr_ = nullptr;
};

class ReceiverRef {
 public:
  ReceiverRef() = default;
  explicit ReceiverRef(const ibmw_transport_receiver_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ReceiverRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_transport_receiver_base_t* NativeHandle() const {
    return base_ptr_;
  }

  /**
   * @brief Subscribe to a certain type
   *
   * @param msg_type_support
   * @param callback
   * @return Subscribe result
   */
  bool OnMessage(
      const ibmw_type_support_base_t* msg_type_support,
      MessageCallback&& callback) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->on_message(base_ptr_->ctx, msg_type_support, callback.NativeHandle());
  }

  /**
   * @brief Subscribe to a certain type via the zero-copy receive fast lane.
   *
   * Phase 7 §7.3 §U: receiver-side counterpart of SenderRef::Loan. The
   * driver delivers the user-payload pointer of a borrowed sample
   * directly to the callback. Returns false if the underlying driver
   * does not implement attach_loaned (i.e. cannot do zero-copy receive).
   * This API returns failure rather than silently using OnMessage.
   *
   * @param msg_type_support
   * @param callback
   * @return Subscribe result (false on driver no-support or rejected binding)
   */
  bool AttachLoaned(
      const ibmw_type_support_base_t* msg_type_support,
      LoanedMessageCallback&& callback) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (base_ptr_->attach_loaned == nullptr) {
      return false;
    }
    return base_ptr_->attach_loaned(base_ptr_->ctx, msg_type_support, callback.NativeHandle());
  }

  bool AttachLoanedRetained(
      const ibmw_type_support_base_t* msg_type_support,
      LoanedMessageCallback&& callback,
      LoanedSubscription* out_subscription,
      RetainedLoanOptions options) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (out_subscription == nullptr || options.max_outstanding == 0 ||
        base_ptr_->attach_loaned_retained == nullptr) {
      return false;
    }

    ibmw_loaned_subscription_base_t* native_subscription = nullptr;
    const bool attached = base_ptr_->attach_loaned_retained(
        base_ptr_->ctx,
        msg_type_support,
        callback.NativeHandle(),
        ibmw_retained_loan_options_t{options.max_outstanding},
        &native_subscription);
    if (!attached || native_subscription == nullptr) {
      if (native_subscription != nullptr &&
          native_subscription->destroy != nullptr) {
        native_subscription->destroy(native_subscription->ctx);
      }
      return false;
    }
    if (native_subscription->stop == nullptr ||
        native_subscription->close == nullptr ||
        native_subscription->destroy == nullptr) {
      if (native_subscription->destroy != nullptr) {
        native_subscription->destroy(native_subscription->ctx);
      }
      return false;
    }

    *out_subscription = LoanedSubscription(native_subscription);
    return true;
  }

  /**
   * @brief Get the topic for current subscriber
   *
   * @return Topic for current subscriber
   */
  std::string_view GetTopic() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return ibmw::util::ToStdStringView(base_ptr_->get_topic(base_ptr_->ctx));
  }

 private:
  const ibmw_transport_receiver_base_t* base_ptr_ = nullptr;
};

class TransportRef {
 public:
  TransportRef() = default;
  explicit TransportRef(const ibmw_transport_handle_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~TransportRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_transport_handle_base_t* NativeHandle() const { return base_ptr_; }

  SenderRef GetPublisher(std::string_view topic) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (base_ptr_->get_sender == nullptr) return SenderRef();
    return SenderRef(
        base_ptr_->get_sender(base_ptr_->ctx, ibmw::util::ToIbmwStringView(topic)));
  }

  ReceiverRef GetSubscriber(std::string_view topic) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    if (base_ptr_->get_receiver == nullptr) return ReceiverRef();
    return ReceiverRef(
        base_ptr_->get_receiver(base_ptr_->ctx, ibmw::util::ToIbmwStringView(topic)));
  }

  void MergeSubscribeContextToPublishContext(
      const ContextRef subscribe_ctx_ref, ContextRef publish_ctx_ref) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->merge_subscribe_context_to_publish_context(
        base_ptr_->ctx,
        subscribe_ctx_ref.NativeHandle(),
        publish_ctx_ref.NativeHandle());
  }

 private:
  const ibmw_transport_handle_base_t* base_ptr_ = nullptr;
};

class SenderAccessBase {
 public:
  explicit SenderAccessBase(SenderRef publisher, std::string_view msg_type_name)
      : publisher_(publisher), msg_type_name_(msg_type_name) {}
  virtual ~SenderAccessBase() = default;

  SenderAccessBase(const SenderAccessBase&) = delete;
  SenderAccessBase& operator=(const SenderAccessBase&) = delete;

  std::shared_ptr<Context> NewContextSharedPtr(ContextRef ctx_ref = ContextRef()) const {
    auto result_ctx = default_ctx_ptr_
                          ? std::make_shared<Context>(*default_ctx_ptr_)
                          : std::make_shared<Context>();
    if (ctx_ref) {
      publisher_.MergeSubscribeContextToPublishContext(ctx_ref, result_ctx);
    }

    return result_ctx;
  }

  void SetDefaultContextSharedPtr(const std::shared_ptr<Context>& ctx_ptr) {
    default_ctx_ptr_ = ctx_ptr;
  }

  std::shared_ptr<Context> GetDefaultContextSharedPtr() const {
    return default_ctx_ptr_;
  }

  std::string_view GetTopic() const {
    return publisher_.GetTopic();
  }

  bool RegisterLoanedPodTopic(
      size_t initial_size_hint,
      size_t alignment = alignof(std::max_align_t)) {
    return publisher_.RegisterLoanedPodTopic(
        msg_type_name_, initial_size_hint, alignment);
  }

 protected:
  void SendImpl(ContextRef ctx_ref, const void* msg_ptr) {
    publisher_.Send(msg_type_name_, ctx_ref, msg_ptr);
  }

  // Phase 7 §7.3 §K-3.4: typed-layer building blocks for the loan fast
  // lane. Subclasses (SenderAccess<T> per transport) wrap these in
  // typed Loan() / Publish() methods that return / accept
  // LoanedSample<T>. The msg_type string used here is the same
  // transport-prefixed name SendImpl uses, so dispatcher lookups land
  // on the same SendTypeWrapper. Loan path intentionally bypasses
  // ctx-meta (pub_seq / pub_ts) and the publish filter chain — see
  // dispatcher.Loan() comment.
  LoanedSampleHandle LoanImpl(size_t requested_size,
                              LoanMode mode = kLoanModePodOnly) {
    return publisher_.Loan(msg_type_name_, requested_size, mode);
  }

  void PublishLoanedImpl(LoanedSampleHandle& handle) {
    publisher_.PublishLoaned(handle);
  }

  void ReleaseLoanedImpl(LoanedSampleHandle& handle) {
    publisher_.ReleaseLoaned(handle);
  }

  /// Accessor for subclasses that want to construct a typed
  /// LoanedSample<T> wrapper directly from the underlying SenderRef.
  SenderRef GetSenderRef() const noexcept { return publisher_; }

 protected:
  SenderRef publisher_;
  const std::string msg_type_name_;
  std::shared_ptr<Context> default_ctx_ptr_;
};

template <typename>
class SenderAccess;

class ReceiverAccessBase {
 public:
  explicit ReceiverAccessBase(ReceiverRef subscriber, std::string_view msg_type_name)
      : subscriber_(subscriber), msg_type_name_(msg_type_name) {}
  virtual ~ReceiverAccessBase() = default;

  ReceiverAccessBase(const ReceiverAccessBase&) = delete;
  ReceiverAccessBase& operator=(const ReceiverAccessBase&) = delete;

  std::string_view GetTopic() const {
    return subscriber_.GetTopic();
  }

 protected:
 protected:
  ReceiverRef subscriber_;
  const std::string msg_type_name_;
};

template <typename>
class ReceiverAccess;

}  // namespace ibmw::transport
