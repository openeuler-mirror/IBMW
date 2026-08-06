// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <vector>
#include "iceoryx_posh/popo/listener.hpp"
#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "base/string_util.h"

#ifndef IBMW_ICEORYX_EXPORT
#define IBMW_ICEORYX_EXPORT __attribute__((visibility("default")))
#endif

namespace ibmw::extensions::iceoryx_extension {

class IoxSubscriptionState;
class IoxSubscriptionCallbackContext;

class IoxNotificationGate {
 public:
  bool TryEnter() noexcept;
  void Exit() noexcept;
  void Close() noexcept;
  void WaitForQuiescence() noexcept;
  size_t Active() const noexcept;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  size_t active_ = 0;
  bool closed_ = false;
};

// This is an in-process URL ownership registry, not backend fan-out. A URL is
// reserved before the iceoryx endpoint is built and committed only after the
// listener attachment succeeds. Failed construction/attachment rolls the
// reservation back so a later registration can retry.
class IoxSubscriptionRegistry {
 public:
  bool Reserve(std::string_view url);
  bool Commit(std::string_view url,
              std::shared_ptr<IoxSubscriptionState> state);
  void Rollback(std::string_view url) noexcept;
  void Remove(std::string_view url,
              const IoxSubscriptionState* expected) noexcept;
  bool Contains(std::string_view url) const;
  size_t Size() const noexcept { return subscriptions_.size(); }
  auto& Entries() noexcept { return subscriptions_; }
  const auto& Entries() const noexcept { return subscriptions_; }
  auto ExtractAll() noexcept {
    decltype(subscriptions_) extracted;
    extracted.swap(subscriptions_);
    reservations_.clear();
    return extracted;
  }

 private:
  std::unordered_map<std::string, std::shared_ptr<IoxSubscriptionState>,
                     ibmw::common::util::StringHash, std::equal_to<>>
      subscriptions_;
  std::unordered_set<std::string, ibmw::common::util::StringHash,
                     std::equal_to<>>
      reservations_;
};

class IoxCallbackContextRegistry {
 public:
  void Add(std::shared_ptr<IoxSubscriptionCallbackContext> context) {
    contexts_.emplace_back(std::move(context));
  }
  bool Remove(const IoxSubscriptionCallbackContext* expected) noexcept;
  size_t Size() const noexcept { return contexts_.size(); }
  auto ExtractAll() noexcept {
    decltype(contexts_) extracted;
    extracted.swap(contexts_);
    return extracted;
  }

 private:
  std::vector<std::shared_ptr<IoxSubscriptionCallbackContext>> contexts_;
};

bool IsIceoryxListenerCallbackThread() noexcept;

struct IoxBusControl {
  std::mutex mutex;
  std::condition_variable registration_cv;
  size_t registrations_in_flight = 0;
  std::shared_ptr<iox::popo::Listener> listener;
  bool shutting_down = false;
  IoxSubscriptionRegistry subscriptions;
  IoxCallbackContextRegistry callback_contexts;
};

// Keeps registration completion accounting alive independently of the bus
// owner. StartLocked must be called while holding IoxBusControl::mutex after a
// URL reservation succeeds.
class IoxRegistrationGuard {
 public:
  explicit IoxRegistrationGuard(std::shared_ptr<IoxBusControl> control) noexcept
      : control_(std::move(control)) {}
  ~IoxRegistrationGuard() { Finish(); }

  IoxRegistrationGuard(const IoxRegistrationGuard&) = delete;
  IoxRegistrationGuard& operator=(const IoxRegistrationGuard&) = delete;

  void StartLocked() noexcept {
    ++control_->registrations_in_flight;
    active_ = true;
  }

  void Finish() noexcept {
    if (!active_) return;
    {
      std::lock_guard<std::mutex> lock(control_->mutex);
      if (control_->registrations_in_flight != 0) {
        --control_->registrations_in_flight;
      }
      active_ = false;
    }
    control_->registration_cv.notify_all();
  }

 private:
  std::shared_ptr<IoxBusControl> control_;
  bool active_ = false;
};

iox::popo::SubscriberOptions MakeSubscriberOptions(bool retained) noexcept;

class IoxReleaseToken {
 public:
  explicit IoxReleaseToken(std::function<void()> release)
      : release_(std::move(release)) {}
  ~IoxReleaseToken() { Release(); }

  IoxReleaseToken(const IoxReleaseToken&) = delete;
  IoxReleaseToken& operator=(const IoxReleaseToken&) = delete;

  void Release() noexcept {
    if (!released_.exchange(true) && release_) release_();
  }

 private:
  std::function<void()> release_;
  std::atomic_bool released_{false};
};

class IoxLeaseLimiter {
 public:
  explicit IoxLeaseLimiter(size_t limit) : limit_(limit) {}

  bool TryAcquire() noexcept {
    if (outstanding_ >= limit_) {
      ++dropped_;
      return false;
    }
    ++outstanding_;
    return true;
  }

  void Release() noexcept {
    if (outstanding_ != 0) --outstanding_;
  }

  size_t Outstanding() const noexcept { return outstanding_; }
  size_t Dropped() const noexcept { return dropped_; }

 private:
  size_t limit_;
  size_t outstanding_ = 0;
  size_t dropped_ = 0;
};

namespace internal {

template <typename Release, typename Complete, typename Dispatch>
void RunIoxAcquiredPayload(Release&& release,
                           Complete&& complete,
                           Dispatch&& dispatch) noexcept {
  bool release_on_exit = true;
  try {
    dispatch(release_on_exit);
  } catch (...) {
  }
  if (release_on_exit) release();
  complete();
}

// Registration rollback may wait for an already-entered listener callback.
// Keep the sequencing in a helper whose contract deliberately has no access
// to IceoryxBus::mutex_.
template <typename CloseGate, typename Detach, typename Wait, typename Remove>
void RunIoxRegistrationRollback(CloseGate&& close_gate,
                                Detach&& detach,
                                Wait&& wait,
                                Remove&& remove) noexcept {
  close_gate();
  detach();
  wait();
  remove();
}

}  // namespace internal

struct IoxSubscriberRegistrationOptions {
  bool retained = false;
  size_t max_outstanding = std::numeric_limits<size_t>::max();
};

struct IoxRawLoanRequest {
  uint32_t payload_size;
  uint32_t alignment;
};

std::optional<IoxRawLoanRequest> MakeIoxRawLoanRequest(
    size_t requested_size, size_t alignment) noexcept;

// Returns the normalized power-of-two growth target accepted by iceoryx, or
// nullopt when min_size cannot fit its uint32_t payload-size ABI.
std::optional<size_t> NextIoxShmSize(size_t current_size,
                                    size_t min_size) noexcept;

class IoxAsyncStop {
 public:
  explicit IoxAsyncStop(std::function<void()> action);
  ~IoxAsyncStop();

  IoxAsyncStop(const IoxAsyncStop&) = delete;
  IoxAsyncStop& operator=(const IoxAsyncStop&) = delete;

  void Request(std::shared_ptr<void> keepalive = {}) noexcept;
  bool Close(std::chrono::milliseconds timeout,
             bool called_from_callback = false) noexcept;
  bool Complete() const noexcept;

 private:
  struct State;
  static void Run(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
  std::thread worker_;
};

class IoxPublisher;

class IoxLoanedShm {
  friend class IoxPublisher;

 public:
  ~IoxLoanedShm();

  IoxLoanedShm(const IoxLoanedShm&) = delete;
  IoxLoanedShm& operator=(const IoxLoanedShm&) = delete;

  void* Ptr() const { return ptr_; }
  size_t Size() const { return size_; }

 private:
  IoxLoanedShm(void* ptr, size_t size, IoxPublisher* owner) noexcept
      : ptr_(ptr), size_(size), owner_(owner) {}

 private:
  void* ptr_;
  size_t size_;
  IoxPublisher* owner_;
};

// Locking and lifecycle accounting are kept in one object so publisher hot
// paths take only the publisher's existing serialization lock. This class is
// independently constructible for shutdown/loan lifecycle tests without RouDi.
class IoxLoanTracker {
 public:
  template <typename Loan>
  bool TryTrackLoan(Loan&& loan) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || !loan()) return false;
    ++outstanding_;
    return true;
  }

  template <typename Complete>
  void CompleteTrackedLoan(Complete&& complete) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      complete();
      if (outstanding_ != 0) --outstanding_;
    }
    cv_.notify_all();
  }

  // The old loan remains outstanding unless replacement fully succeeds. The
  // caller must therefore acquire the new chunk before releasing the old one.
  // Either outcome leaves exactly one tracked loan for the RAII owner.
  template <typename Replace>
  bool ReplaceTrackedLoan(Replace&& replace) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outstanding_ == 0) return false;
    return replace();
  }

  void BeginShutdown() noexcept;
  bool WaitForLoans(std::chrono::milliseconds timeout) noexcept;
  bool IsClosing() const noexcept;
  size_t Outstanding() const noexcept;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool closing_ = false;
  size_t outstanding_ = 0;
};

class IoxPublisher {
  friend class IoxLoanedShm;

 public:
  IoxPublisher(std::string_view url, size_t shm_size);
  ~IoxPublisher() = default;

  IoxPublisher(const IoxPublisher&) = delete;
  IoxPublisher& operator=(const IoxPublisher&) = delete;

  IoxLoanedShm LoanShm(size_t min_size);
  void UpdateLoanShm(IoxLoanedShm& loaned_shm, size_t min_size);
  void SendShm(IoxLoanedShm& loaned_shm);

  // ----- Zero-copy POD-only path -----
  // These bypass IoxLoanedShm RAII (caller owns the lifecycle via the raw
  // chunk pointer), so they integrate with the core LoanedSampleHandle API.
  // LoanRaw returns nullptr on failure. The returned pointer is valid until
  // PublishRaw or ReleaseRaw is called.
  void* LoanRaw(size_t requested_size,
                size_t alignment = iox::CHUNK_DEFAULT_USER_PAYLOAD_ALIGNMENT);
  void PublishRaw(void* chunk_ptr) noexcept;
  void ReleaseRaw(void* chunk_ptr) noexcept;

  void BeginShutdown() noexcept { loan_tracker_.BeginShutdown(); }
  bool WaitForLoans(std::chrono::milliseconds timeout) noexcept {
    return loan_tracker_.WaitForLoans(timeout);
  }

 private:
  void ReleaseShm(IoxLoanedShm& loaned_shm) noexcept;

  IoxLoanTracker loan_tracker_;
  size_t shm_size_;
  iox::popo::UntypedPublisher publisher_;
};

inline IoxLoanedShm::~IoxLoanedShm() {
  if (owner_ != nullptr) owner_->ReleaseShm(*this);
}

class IBMW_ICEORYX_EXPORT IceoryxBus {
 public:
  using MsgHandleFunc = std::function<void(
      const void* payload, size_t size, std::function<void()>&& release)>;

 public:
  IceoryxBus();
  ~IceoryxBus();

  IceoryxBus(const IceoryxBus&) = delete;
  IceoryxBus& operator=(const IceoryxBus&) = delete;

  void Initialize(uint64_t shm_init_size, std::string_view runtime_id);
  // Must be called by the runtime lifecycle owner, never from a listener
  // callback. Callback-thread calls are rejected promptly without teardown.
  bool TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept;
  void Shutdown();

  void RegisterPublisher(std::string_view url, size_t initial_size_hint = 0);
  std::shared_ptr<IoxSubscriptionState> RegisterSubscriber(
      std::string_view url,
      MsgHandleFunc&& handle,
      IoxSubscriberRegistrationOptions options = {});
  // Current phase intentionally permits only one in-process subscriber per
  // exact URL. Registering the same URL again fails; fan-out is phase-two work.

  IoxPublisher* GetPublisher(std::string_view url);

 private:
  uint64_t shm_init_size_;

  std::mutex mutex_;
  std::shared_ptr<IoxBusControl> control_;
  bool prepared_ = false;

  std::shared_ptr<iox::popo::Listener> iox_listener_ptr_;
  std::unordered_map<
      std::string, std::unique_ptr<IoxPublisher>,
      ibmw::common::util::StringHash, std::equal_to<>>
      iox_pub_catalog_;
};

class IoxSubscriptionState
    : public std::enable_shared_from_this<IoxSubscriptionState> {
 public:
  IoxSubscriptionState(std::string_view url,
                       IceoryxBus::MsgHandleFunc callback,
                       IoxSubscriberRegistrationOptions options,
                       std::shared_ptr<IoxBusControl> control);
  ~IoxSubscriptionState();

  IoxSubscriptionState(const IoxSubscriptionState&) = delete;
  IoxSubscriptionState& operator=(const IoxSubscriptionState&) = delete;

  iox::popo::UntypedSubscriber& Subscriber() noexcept { return *subscriber_; }
  void OnData() noexcept;
  void SetCallbackContext(
      std::shared_ptr<IoxSubscriptionCallbackContext> context) noexcept;
  void Stop() noexcept;
  bool Close(std::chrono::milliseconds timeout) noexcept;

 private:
  void DetachFromListener() noexcept;
  void Deregister() noexcept;
  void Release(const void* payload) noexcept;
  bool Quiescent() const noexcept;
  bool WaitForQuiescence() noexcept;

  std::shared_ptr<IoxBusControl> control_;
  std::string url_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::unique_ptr<iox::popo::UntypedSubscriber> subscriber_;
  IceoryxBus::MsgHandleFunc callback_;
  IoxLeaseLimiter lease_limiter_;
  bool retained_ = false;
  std::shared_ptr<IoxSubscriptionCallbackContext> callback_context_;
  // Declared after every owner resource used by its action so its destructor
  // stops and joins the worker before those resources are destroyed.
  IoxAsyncStop async_stop_;
  size_t active_callbacks_ = 0;
  bool stopping_ = false;
};

// Listener notification data points here rather than at the subscription.
// Bus control keeps every context alive until the Listener itself is destroyed,
// so even an entrance racing detach can only observe a closed gate, never a
// freed state pointer.
class IoxSubscriptionCallbackContext {
 public:
  explicit IoxSubscriptionCallbackContext(
      std::weak_ptr<IoxSubscriptionState> state)
      : state_(std::move(state)) {}

  void Invoke() noexcept;
  void Close() noexcept { gate_.Close(); }
  void WaitForQuiescence() noexcept { gate_.WaitForQuiescence(); }

 private:
  IoxNotificationGate gate_;
  std::weak_ptr<IoxSubscriptionState> state_;
};

}  // namespace ibmw::extensions::iceoryx_extension
