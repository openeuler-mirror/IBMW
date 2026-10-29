// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/string_util.h"
#include "ibmw_selfdev/shm/shm.h"

#ifndef IBMW_SHM_EXPORT
#define IBMW_SHM_EXPORT __attribute__((visibility("default")))
#endif

// Bus layer of the self-developed shm transport. It owns the selfdev
// publishers, subscribers and listener threads of one extension instance and
// implements the lifecycle rules the IBMW transport driver contract needs
// (operation gate, loan accounting, retained leases, two-phase shutdown).
// Semantics follow the iceoryx extension bus; there is no dependency on
// iceoryx and no daemon.
namespace ibmw::extensions::shm_extension {

namespace shm = ibmw::selfdev::shm;

// True while the current thread runs a subscription callback of any ShmBus.
IBMW_SHM_EXPORT bool IsShmCallbackThread() noexcept;

// Next loan size for a framed message: doubles `current` until it holds
// `min_size`. nullopt when the result exceeds the largest selfdev class.
IBMW_SHM_EXPORT std::optional<size_t> NextShmLoanSize(size_t current, size_t min_size) noexcept;

// Admits hot-path operations until Close(); Close() + WaitIdle() lets shutdown
// observe that no Send/Loan is still running.
class IBMW_SHM_EXPORT ShmOperationGate {
 public:
  class Lease {
   public:
    Lease() noexcept = default;
    ~Lease() { Reset(); }
    Lease(Lease&& other) noexcept : gate_(other.gate_) { other.gate_ = nullptr; }
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        Reset();
        gate_ = other.gate_;
        other.gate_ = nullptr;
      }
      return *this;
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    explicit operator bool() const noexcept { return gate_ != nullptr; }

   private:
    friend class ShmOperationGate;
    explicit Lease(ShmOperationGate* gate) noexcept : gate_(gate) {}
    void Reset() noexcept;
    ShmOperationGate* gate_ = nullptr;
  };

  Lease TryAcquire() noexcept;
  void Close() noexcept;
  bool Accepting() const noexcept { return accepting_.load(std::memory_order_seq_cst); }
  bool WaitIdle(std::chrono::steady_clock::time_point deadline) noexcept;
  size_t Active() const noexcept { return active_.load(std::memory_order_acquire); }

 private:
  void Release() noexcept;
  std::atomic_bool accepting_{true};
  std::atomic_size_t active_{0};
  std::mutex mu_;
  std::condition_variable cv_;
};

struct ShmDeliveryOptions {
  // Lossless: publisher waits for consumers, subscribers block the producer.
  bool lossless = false;
  uint32_t queue_capacity = 1;
  // Upper bound for one lossless publish; zero waits indefinitely.
  std::chrono::milliseconds publish_block_timeout{0};
};

struct ShmBusOptions {
  std::string domain;      // empty: "default" (or $IBMW_SHM_DOMAIN)
  std::string root_dir;    // empty: $IBMW_SHM_ROOT, else /tmp
  std::string runtime_name;
  uint64_t shm_init_size = 1024;  // first loan size of a framed Send()
  uint64_t max_bytes_per_publisher = 512ull << 20;
  bool prefault = false;
  std::string listener_thread_name = "ibmw_shm_lsn";
};

class ShmPublisher;

// Loan bookkeeping for handle loans lives in a fixed table sized to the
// per-publisher loan limit, so loaning never allocates. The driver token
// handed out through LoanedSampleHandle is the slot address with the low
// bits of the slot's loan generation in its alignment bits: a stale copy of
// a handle given back is refused even after the slot (and the chunk at the
// same address) is loaned again, unless exactly a multiple of kTagCount
// loans went through that slot in between.
struct alignas(64) ShmLoanSlot {
  static constexpr uintptr_t kTagCount = 64;

  ShmPublisher* owner = nullptr;
  // state = generation << 2 | kFree / kBusy / kLoaned.
  std::atomic_uint32_t state{0};
  std::atomic<void*> chunk{nullptr};  // start of the selfdev user payload

  void* Chunk() const noexcept { return chunk.load(std::memory_order_relaxed); }
  // Token of the current loan; only meaningful while the slot is loaned.
  void* Token() const noexcept {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) |
                                   ((state.load(std::memory_order_relaxed) >> 2) % kTagCount));
  }
  static ShmLoanSlot* FromToken(void* token) noexcept {
    return reinterpret_cast<ShmLoanSlot*>(reinterpret_cast<uintptr_t>(token) & ~(kTagCount - 1));
  }
};
static_assert(alignof(ShmLoanSlot) >= ShmLoanSlot::kTagCount);

class IBMW_SHM_EXPORT ShmPublisher {
 public:
  ShmPublisher(std::string url, const ShmBusOptions& bus_options,
               const ShmDeliveryOptions& delivery, size_t size_hint);
  ~ShmPublisher() = default;

  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;

  bool Valid() const noexcept { return publisher_.Valid(); }
  shm::Error InitError() const noexcept { return publisher_.InitError(); }
  const std::string& Url() const noexcept { return url_; }

  // Tracked raw loans; nullptr on failure (closing, pool or loan limit).
  void* Loan(size_t size, size_t alignment = shm::kDefaultPayloadAlignment) noexcept;
  void Publish(void* chunk) noexcept;
  void Release(void* chunk) noexcept;

  // Loans with a token slot (for LoanedSampleHandle::driver_token).
  ShmLoanSlot* LoanSlot(size_t size, size_t alignment = shm::kDefaultPayloadAlignment) noexcept;
  // Take a slot token (ShmLoanSlot::Token). No-ops for a token of another
  // publisher or of a loan already given back.
  void PublishSlot(void* token) noexcept;
  void ReleaseSlot(void* token) noexcept;

  // Loan size that most recently fitted a framed Send(); grows only.
  size_t SizeHint() const noexcept { return size_hint_.load(std::memory_order_relaxed); }
  void RaiseSizeHint(size_t size) noexcept;

  void BeginShutdown() noexcept;
  bool IsClosing() const noexcept { return closing_.load(std::memory_order_acquire); }
  bool WaitForLoans(std::chrono::steady_clock::time_point deadline) noexcept;
  size_t Outstanding() const noexcept { return outstanding_.load(std::memory_order_acquire); }
  bool HasSubscribers() const noexcept { return publisher_.HasSubscribers(); }
  shm::PublisherStats Stats() const noexcept { return publisher_.Stats(); }

 private:
  bool BeginLoan() noexcept;
  void EndLoan() noexcept;
  void* TakeSlot(void* token) noexcept;

  std::string url_;
  std::atomic_size_t size_hint_;
  std::atomic_bool closing_{false};
  std::atomic_size_t outstanding_{0};
  std::mutex wait_mu_;
  std::condition_variable wait_cv_;
  std::array<ShmLoanSlot, shm::kMaxChunksAllocatedPerPublisher> slots_;
  shm::Publisher publisher_;
};

using ShmMessageHandler = std::function<void(
    const void* payload, size_t size, std::function<void()>&& release)>;

struct ShmSubscriptionOptions {
  // Retained: the release functor may outlive the callback; at most
  // max_outstanding samples are held, further samples are dropped and never
  // replayed. Otherwise the sample is released when the callback returns.
  bool retained = false;
  size_t max_outstanding = 1;
  // URL reserved beforehand with ShmBus::ReserveSubscriberUrl().
  bool url_reserved = false;
};

class ShmListenerPool;
struct ShmSubscriptionRegistry;

class IBMW_SHM_EXPORT ShmSubscription
    : public std::enable_shared_from_this<ShmSubscription> {
 public:
  ShmSubscription(std::string url, ShmMessageHandler handler,
                  ShmSubscriptionOptions options, const ShmDeliveryOptions& delivery,
                  std::shared_ptr<ShmSubscriptionRegistry> registry);
  ~ShmSubscription() = default;

  ShmSubscription(const ShmSubscription&) = delete;
  ShmSubscription& operator=(const ShmSubscription&) = delete;

  bool Valid() const noexcept { return subscriber_.Valid(); }
  shm::Error InitError() const noexcept { return subscriber_.InitError(); }
  const std::string& Url() const noexcept { return url_; }

  // Non-blocking: no callback starts after Stop() returns; queued samples are
  // discarded so lossless producers are not held up. Retained samples stay
  // valid until released.
  void Stop() noexcept;
  // Stop(), then wait until no callback runs and every retained sample is
  // released; detaches from the listener and frees the URL. False on timeout
  // or when called from this subscription's own callback.
  bool Close(std::chrono::milliseconds timeout) noexcept;
  // Close() without waiting: succeeds only if already quiescent.
  bool TryFinalize() noexcept;

  bool Stopping() const noexcept;
  bool Finalized() const noexcept { return finalized_.load(std::memory_order_acquire); }
  size_t Outstanding() const noexcept;
  uint64_t Delivered() const noexcept { return delivered_.load(std::memory_order_relaxed); }
  uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  shm::SubscriberStats Stats() const noexcept { return subscriber_.Stats(); }
  bool HasPublisher() const noexcept { return subscriber_.Stats().connections != 0; }

 private:
  friend class ShmListenerPool;
  class ReleaseToken;

  static void OnEvent(shm::Subscriber* subscriber, void* context);
  void Drain() noexcept;
  void Deliver(const void* payload) noexcept;
  void ReleaseSample(const void* payload, bool leased) noexcept;
  void DiscardQueued() noexcept;
  bool QuiescentLocked() const noexcept { return active_callbacks_ == 0 && outstanding_ == 0; }
  void Finalize() noexcept;

  std::string url_;
  ShmMessageHandler handler_;
  ShmSubscriptionOptions options_;
  std::shared_ptr<ShmSubscriptionRegistry> registry_;
  std::atomic_uint64_t delivered_{0};
  std::atomic_uint64_t dropped_{0};
  std::atomic_bool finalized_{false};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  size_t active_callbacks_ = 0;
  size_t outstanding_ = 0;

  std::mutex attach_mu_;
  std::shared_ptr<shm::Listener> listener_;  // set while attached
  // Declared last: destroyed first, which detaches it from the listener and
  // waits for a running callback before the members above go away.
  shm::Subscriber subscriber_;
};

// Listener threads shared by every subscription of a bus. A new thread is
// started only when the current ones are full.
class IBMW_SHM_EXPORT ShmListenerPool {
 public:
  explicit ShmListenerPool(std::string thread_name) : thread_name_(std::move(thread_name)) {}
  void SetThreadName(std::string thread_name);
  bool Attach(const std::shared_ptr<ShmSubscription>& subscription);
  size_t Threads() const;
  void Clear() noexcept;
  // Leaks the listener threads without joining them (a callback may never
  // return); used when shutdown could not quiesce the bus.
  void Abandon() noexcept;

 private:
  std::string thread_name_;
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<shm::Listener>> listeners_;
};

// URL -> subscription. A URL is reserved first, then committed; at most one
// subscription per URL (same as the iceoryx bus).
struct ShmSubscriptionRegistry {
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<ShmSubscription>,
                     ibmw::common::util::StringHash, std::equal_to<>>
      subscriptions;
  std::unordered_set<std::string, ibmw::common::util::StringHash, std::equal_to<>> reserved;
  bool shutting_down = false;

  void Remove(std::string_view url, const ShmSubscription* expected) noexcept;
};

class IBMW_SHM_EXPORT ShmBus {
 public:
  ShmBus();
  ~ShmBus();

  ShmBus(const ShmBus&) = delete;
  ShmBus& operator=(const ShmBus&) = delete;

  // Throws std::runtime_error when the selfdev runtime cannot start.
  void Initialize(const ShmBusOptions& options);
  void SetDeliveryOptions(const ShmDeliveryOptions& options) noexcept;
  ShmDeliveryOptions DeliveryOptions() const noexcept;
  const ShmBusOptions& Options() const noexcept { return options_; }
  void SetListenerThreadName(std::string name);

  ShmOperationGate::Lease TryAcquireOperation() noexcept { return gate_->TryAcquire(); }

  // Creates the publisher of `url` or returns the existing one. Throws on
  // failure or after shutdown started.
  ShmPublisher* RegisterPublisher(std::string_view url, size_t size_hint = 0,
                                  bool* created = nullptr);
  // nullptr without a lease, after shutdown started, or when unknown.
  ShmPublisher* GetPublisher(const ShmOperationGate::Lease& lease, std::string_view url) const;
  void RollbackPublisher(std::string_view url, const ShmPublisher* expected) noexcept;

  // Throws when the URL is taken (or not reserved with url_reserved), the
  // subscriber cannot be created or shutdown started.
  std::shared_ptr<ShmSubscription> RegisterSubscriber(
      std::string_view url, ShmMessageHandler handler,
      ShmSubscriptionOptions options = {});
  bool ReserveSubscriberUrl(std::string_view url);
  void RollbackSubscriberUrl(std::string_view url) noexcept;

  // Phase one: stop intake, stop callbacks, wait for loans and retained
  // samples. Retryable; never frees resources. Rejected on a callback thread.
  bool TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept;
  // Phase two: frees everything. Requires a successful phase one. If the bus
  // is destroyed without it and cannot be prepared, publishers, subscriptions,
  // listener threads and the gate are leaked rather than freed under
  // threads that still use them. The bus object's own members (options,
  // mutexes) still die with it, so an owner that cannot prepare should leak
  // the whole bus instead of destroying it, as ShmExtension does.
  bool Shutdown() noexcept;
  bool Prepared() const noexcept;

  size_t PublisherCount() const;
  size_t SubscriptionCount() const;
  size_t ListenerThreads() const { return listeners_.Threads(); }

 private:
  // Frees the URL if its previous subscription was stopped and is quiescent.
  bool ReclaimStoppedLocked(std::string_view url) noexcept;

  ShmBusOptions options_;
  // Heap-held so a failed destruction can leak it (leases point into it).
  std::unique_ptr<ShmOperationGate> gate_ = std::make_unique<ShmOperationGate>();

  mutable std::mutex lifecycle_mu_;
  bool initialized_ = false;
  bool prepared_ = false;
  bool shut_down_ = false;
  ShmDeliveryOptions delivery_;

  mutable std::shared_mutex pub_mu_;
  bool pub_closed_ = false;
  std::unordered_map<std::string, std::unique_ptr<ShmPublisher>,
                     ibmw::common::util::StringHash, std::equal_to<>>
      publishers_;
  std::shared_ptr<ShmSubscriptionRegistry> registry_;
  ShmListenerPool listeners_;
};

}  // namespace ibmw::extensions::shm_extension
