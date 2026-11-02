// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "shm_ext/shm_bus.h"

#include <algorithm>
#include <charconv>
#include <stdexcept>

#include "shm_ext/global.h"

namespace ibmw::extensions::shm_extension {

namespace {

// Subscription whose callback runs on this thread (innermost), if any.
thread_local const ShmSubscription* t_current_subscription = nullptr;

class CallbackScope {
 public:
  explicit CallbackScope(const ShmSubscription* s) noexcept : previous_(t_current_subscription) {
    t_current_subscription = s;
  }
  ~CallbackScope() { t_current_subscription = previous_; }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  const ShmSubscription* previous_;
};

constexpr size_t kMaxLoanSize = size_t{1} << shm::kMaxClassShift;

std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point deadline) {
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  return std::max(left, std::chrono::milliseconds(0));
}

// Longest sleep of a shutdown wait. Releases on the fast path do not notify
// (see ShmOperationGate::Release), so this also bounds how late a waiter sees
// the count reach zero.
constexpr std::chrono::milliseconds kWaitSlice{1};

// Waits on a steady deadline with relative slices. wait_until(steady) is not
// used: libc++ on musl has no pthread_cond_clockwait and turns it into a wall
// clock wait, which a clock step moves. Here the deadline stays steady; a step
// backward can only stretch the one slice it lands in (wait_for has the same
// wall clock underneath on that libc++).
template <class Predicate>
bool WaitWithDeadline(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                      std::chrono::steady_clock::time_point deadline, Predicate predicate) {
  while (!predicate()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    cv.wait_for(lock, std::min<std::chrono::steady_clock::duration>(deadline - now, kWaitSlice));
  }
  return true;
}

// Set once a listener thread applied ShmListenerPool's thread setup.
thread_local bool t_listener_thread_ready = false;

}  // namespace

bool IsShmCallbackThread() noexcept { return t_current_subscription != nullptr; }

std::optional<size_t> NextShmLoanSize(size_t current, size_t min_size) noexcept {
  if (min_size > kMaxLoanSize) return std::nullopt;
  size_t size = std::clamp<size_t>(current, 1, kMaxLoanSize);
  while (size < min_size) size = std::min(size * 2, kMaxLoanSize);
  return size;
}

std::optional<gid_t> ParseShmGroupId(std::string_view text) noexcept {
  uint64_t value = 0;
  const char* const end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (text.empty() || ec != std::errc() || ptr != end) return std::nullopt;
  const auto gid = static_cast<gid_t>(value);
  if (static_cast<uint64_t>(gid) != value || gid == static_cast<gid_t>(-1)) return std::nullopt;
  return gid;
}

// ============================================================ gate

void ShmOperationGate::Lease::Reset() noexcept {
  if (gate_ != nullptr) gate_->Release();
  gate_ = nullptr;
}

// The gate and the loan count use "publish my flag, then read the other's"
// on both sides (Dekker); that needs seq_cst, acquire/release alone allows
// both sides to miss each other.
//
// The first load only refuses early: callers that keep retrying after Close()
// must not keep the count from ever reading zero. Admission is decided by the
// load after the increment.
ShmOperationGate::Lease ShmOperationGate::TryAcquire() noexcept {
  if (!accepting_.load(std::memory_order_seq_cst)) return Lease();
  active_.fetch_add(1, std::memory_order_seq_cst);
  if (!accepting_.load(std::memory_order_seq_cst)) {
    Release();
    return Lease();
  }
  return Lease(this);
}

// While the gate is open nobody can be waiting yet, so the mutex is skipped:
// this gate is shared by every topic of the bus. The decrement is the last
// access to the gate, so a waiter that sees zero and destroys it cannot overtake
// this path. If Close() lands between the load and the decrement, the missing
// notify costs the waiter at most one kWaitSlice.
//
// After Close() the slow path decrements under the mutex: WaitIdle checks its
// predicate under it too, so a waiter that sees zero cannot overtake a releaser
// still about to notify.
void ShmOperationGate::Release() noexcept {
  if (accepting_.load(std::memory_order_seq_cst)) [[likely]] {
    active_.fetch_sub(1, std::memory_order_seq_cst);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (active_.fetch_sub(1, std::memory_order_seq_cst) == 1 && !accepting_.load(std::memory_order_seq_cst)) {
    cv_.notify_all();
  }
}

void ShmOperationGate::Close() noexcept { accepting_.store(false, std::memory_order_seq_cst); }

void ShmOperationGate::Reopen() noexcept { accepting_.store(true, std::memory_order_seq_cst); }

bool ShmOperationGate::WaitIdle(std::chrono::steady_clock::time_point deadline) noexcept {
  std::unique_lock<std::mutex> lock(mu_);
  return WaitWithDeadline(cv_, lock, deadline, [this] { return active_.load(std::memory_order_seq_cst) == 0; });
}

// ============================================================ publisher

shm::PublisherOptions MakeShmPublisherOptions(const ShmBusOptions& bus, const ShmDeliveryOptions& delivery) {
  shm::PublisherOptions o;
  o.too_slow = delivery.lossless ? shm::TooSlowPolicy::kWaitForConsumer : shm::TooSlowPolicy::kDiscardOldest;
  o.max_bytes = bus.max_bytes_per_publisher;
  o.prefault = bus.prefault;
  o.block_timeout = delivery.publish_block_timeout;
  o.require_read_only = bus.require_read_only;
  return o;
}

namespace {

// The iceoryx bus truncated long names (distinct topics could collide);
// shm refuses them with a clear error instead.
void CheckUrlLength(std::string_view url) {
  if (url.size() > shm::kMaxUrlLength) {
    throw std::runtime_error("shm URL of " + std::to_string(url.size()) + " bytes exceeds the limit of " +
                             std::to_string(shm::kMaxUrlLength) + " (encoded topic + type): " +
                             std::string(url.substr(0, 128)) + "...");
  }
}

}  // namespace

ShmPublisher::ShmPublisher(std::string url, const ShmBusOptions& bus_options, const ShmDeliveryOptions& delivery,
                           size_t size_hint)
    : url_(std::move(url)),
      size_hint_(size_hint),
      publisher_(url_, MakeShmPublisherOptions(bus_options, delivery)) {
  for (auto& slot : slots_) slot.owner = this;
}

namespace {
constexpr uint32_t kSlotFree = 0, kSlotBusy = 1, kSlotLoaned = 2;
}  // namespace

bool ShmPublisher::BeginLoan() noexcept {
  if (closing_.load(std::memory_order_seq_cst)) return false;  // see TryAcquire
  outstanding_.fetch_add(1, std::memory_order_seq_cst);
  if (closing_.load(std::memory_order_seq_cst)) {
    EndLoan();
    return false;
  }
  return true;
}

// Same two paths as ShmOperationGate::Release: handle publishes hold no gate
// lease, so once WaitForLoans sees zero the publisher may be destroyed right
// away, and nothing touches it after the decrement here.
void ShmPublisher::EndLoan() noexcept {
  if (!closing_.load(std::memory_order_seq_cst)) [[likely]] {
    outstanding_.fetch_sub(1, std::memory_order_seq_cst);
    return;
  }
  std::lock_guard<std::mutex> lock(wait_mu_);
  if (outstanding_.fetch_sub(1, std::memory_order_seq_cst) == 1 && closing_.load(std::memory_order_seq_cst)) {
    wait_cv_.notify_all();
  }
}

void* ShmPublisher::Loan(size_t size, size_t alignment) noexcept {
  if (size > kMaxLoanSize || alignment == 0 || alignment > shm::kMaxPayloadAlignment ||
      (alignment & (alignment - 1)) != 0) {
    return nullptr;
  }
  if (!BeginLoan()) return nullptr;
  auto r = publisher_.Loan(static_cast<uint32_t>(size), static_cast<uint32_t>(alignment));
  if (!r) {
    EndLoan();
    MW_DEBUG("shm loan of {} bytes on '{}' failed: {}", size, url_, shm::ErrorName(r.error()));
    return nullptr;
  }
  return r.value();
}

// Only a chunk that really was on loan ends one: a stale or foreign pointer
// must not drive the count (which gates shutdown) below the live loans.
bool ShmPublisher::Publish(void* chunk) noexcept {
  if (!publisher_.Publish(chunk)) return false;
  EndLoan();
  return true;
}

void ShmPublisher::Release(void* chunk) noexcept {
  if (publisher_.Release(chunk)) EndLoan();
}

ShmLoanSlot* ShmPublisher::LoanSlot(size_t size, size_t alignment) noexcept {
  void* chunk = Loan(size, alignment);
  if (chunk == nullptr) return nullptr;
  // At most kMaxChunksAllocatedPerPublisher loans exist, so a free slot is
  // always found.
  for (auto& slot : slots_) {
    uint32_t s = slot.state.load(std::memory_order_relaxed);
    if ((s & 3u) != kSlotFree) continue;
    const uint32_t next = (s & ~3u) + 4u;  // new generation
    if (slot.state.compare_exchange_strong(s, next | kSlotBusy, std::memory_order_acq_rel)) {
      slot.chunk.store(chunk, std::memory_order_relaxed);
      slot.state.store(next | kSlotLoaned, std::memory_order_release);
      return &slot;
    }
  }
  Release(chunk);
  return nullptr;
}

void* ShmPublisher::TakeSlot(void* token) noexcept {
  ShmLoanSlot* slot = ShmLoanSlot::FromToken(token);
  if (slot < slots_.data() || slot >= slots_.data() + slots_.size()) return nullptr;
  const uintptr_t tag = reinterpret_cast<uintptr_t>(token) % ShmLoanSlot::kTagCount;
  // Only one caller gives a loan back; a stale, duplicate or older-generation
  // token gets nothing. chunk is stable while this generation is loaned: it
  // is rewritten only after the slot went free, which fails the CAS below.
  uint32_t s = slot->state.load(std::memory_order_acquire);
  if ((s & 3u) != kSlotLoaned || (s >> 2) % ShmLoanSlot::kTagCount != tag) return nullptr;
  void* chunk = slot->chunk.load(std::memory_order_relaxed);
  if (!slot->state.compare_exchange_strong(s, (s & ~3u) | kSlotFree, std::memory_order_acq_rel)) return nullptr;
  return chunk;
}

bool ShmPublisher::PublishSlot(void* token) noexcept {
  void* chunk = TakeSlot(token);
  return chunk != nullptr && Publish(chunk);
}

void ShmPublisher::ReleaseSlot(void* token) noexcept {
  if (void* chunk = TakeSlot(token)) Release(chunk);
}

void ShmPublisher::RaiseSizeHint(size_t size) noexcept {
  size_t cur = size_hint_.load(std::memory_order_relaxed);
  while (cur < size && !size_hint_.compare_exchange_weak(cur, size, std::memory_order_relaxed)) {
  }
}

void ShmPublisher::BeginShutdown() noexcept {
  closing_.store(true, std::memory_order_seq_cst);
  // A lossless Send() blocked on a consumer that never drains (e.g. a stalled
  // remote process) would otherwise keep its gate lease and stall shutdown.
  publisher_.StopBlocking();
}

bool ShmPublisher::WaitForLoans(std::chrono::steady_clock::time_point deadline) noexcept {
  std::unique_lock<std::mutex> lock(wait_mu_);
  return WaitWithDeadline(wait_cv_, lock, deadline,
                          [this] { return outstanding_.load(std::memory_order_seq_cst) == 0; });
}

// ============================================================ subscription

void ShmSubscriptionRegistry::Remove(std::string_view url, const ShmSubscription* expected) noexcept {
  std::lock_guard<std::mutex> lock(mu);
  auto it = subscriptions.find(url);
  if (it != subscriptions.end() && it->second.get() == expected) subscriptions.erase(it);
}

// Releases a retained sample exactly once: on the first call, or when the
// last copy of the release functor is destroyed.
class ShmSubscription::ReleaseToken {
 public:
  ReleaseToken(std::shared_ptr<ShmSubscription> owner, const void* payload) noexcept
      : owner_(std::move(owner)), payload_(payload) {}
  ~ReleaseToken() { Release(); }
  ReleaseToken(const ReleaseToken&) = delete;
  ReleaseToken& operator=(const ReleaseToken&) = delete;

  void Release() noexcept {
    if (!released_.exchange(true, std::memory_order_acq_rel)) owner_->ReleaseSample(payload_, true);
  }

 private:
  std::shared_ptr<ShmSubscription> owner_;
  const void* payload_;
  std::atomic_bool released_{false};
};

namespace {

shm::SubscriberOptions MakeSubscriberOptions(const ShmSubscriptionOptions& o, const ShmDeliveryOptions& delivery) {
  shm::SubscriberOptions so;
  so.queue_capacity = 1;
  so.queue_full = shm::QueueFullPolicy::kDiscardOldest;
  if (delivery.lossless) {
    so.queue_capacity = delivery.queue_capacity;
    so.queue_full = shm::QueueFullPolicy::kBlockProducer;
  }
  // One spare held slot lets a sample over the lease limit be taken and
  // dropped, as iceoryx does, instead of staying queued.
  so.max_held = o.retained ? static_cast<uint32_t>(std::min<size_t>(o.max_outstanding + 1, shm::kMaxChunksHeldPerSubscriber))
                           : 1;
  return so;
}

}  // namespace

ShmSubscription::ShmSubscription(std::string url, ShmMessageHandler handler, ShmSubscriptionOptions options,
                                 const ShmDeliveryOptions& delivery,
                                 std::shared_ptr<ShmSubscriptionRegistry> registry)
    : url_(std::move(url)),
      handler_(std::move(handler)),
      options_(options),
      registry_(std::move(registry)),
      subscriber_(url_, MakeSubscriberOptions(options, delivery)) {}

void ShmSubscription::OnEvent(shm::Subscriber*, void* context) {
  // The listener detaches the subscriber before ShmSubscription's members
  // are destroyed, so `raw` is valid; lock() fails once destruction began.
  auto* raw = static_cast<ShmSubscription*>(context);
  if (!t_listener_thread_ready) [[unlikely]] {
    t_listener_thread_ready = true;
    if (raw->thread_setup_) {
      try {
        (*raw->thread_setup_)();
      } catch (const std::exception& e) {
        MW_ERROR("Cannot apply the shm listener thread options: {}", e.what());
      } catch (...) {
        MW_ERROR("Cannot apply the shm listener thread options: unknown exception.");
      }
    }
  }
  if (auto self = raw->weak_from_this().lock()) self->Drain();
}

void ShmSubscription::Drain() noexcept {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopping_) break;
      ++active_callbacks_;
    }
    bool more = false;
    auto taken = subscriber_.Take();
    if (taken) {
      more = true;
      Deliver(taken.value());
    } else if (options_.retained && taken.error() == shm::Error::kTooManyHeld) {
      // The listener fires once per new sample, so nothing would come back for
      // this one: ReleaseSample picks it up. If a release just raced past, the
      // slot it freed is usable right now.
      std::lock_guard<std::mutex> lock(mu_);
      if (outstanding_ < options_.max_outstanding) {
        more = true;
      } else {
        starved_ = true;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      --active_callbacks_;
    }
    cv_.notify_all();
    if (!more) return;
  }
  DiscardQueued();
}

void ShmSubscription::Deliver(const void* payload) noexcept {
  // The size Take() validated; the chunk header itself is publisher-writable.
  const size_t size = subscriber_.PayloadSize(payload);
  if (!options_.retained) {
    struct ScopedRelease {
      ShmSubscription* owner;
      const void* payload;
      bool released = false;
    } scoped{this, payload};
    // Valid only during the callback, like the iceoryx bus.
    std::function<void()> release([s = &scoped] {
      if (!s->released) {
        s->released = true;
        s->owner->ReleaseSample(s->payload, false);
      }
    });
    delivered_.fetch_add(1, std::memory_order_relaxed);
    try {
      CallbackScope scope(this);
      handler_(payload, size, std::move(release));
    } catch (const std::exception& e) {
      MW_WARN("shm subscriber callback for '{}' threw: {}", url_, e.what());
    } catch (...) {
      MW_WARN("shm subscriber callback for '{}' threw an unknown exception.", url_);
    }
    if (!scoped.released) ReleaseSample(payload, false);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (outstanding_ >= options_.max_outstanding) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      subscriber_.Release(payload);
      return;
    }
    ++outstanding_;
  }
  std::shared_ptr<ReleaseToken> token;
  try {
    token = std::make_shared<ReleaseToken>(shared_from_this(), payload);
  } catch (...) {
    ReleaseSample(payload, true);
    MW_WARN("shm retained delivery on '{}' failed to allocate a release token.", url_);
    return;
  }
  delivered_.fetch_add(1, std::memory_order_relaxed);
  try {
    CallbackScope scope(this);
    handler_(payload, size, std::function<void()>([token] { token->Release(); }));
  } catch (const std::exception& e) {
    MW_WARN("shm retained callback for '{}' threw: {}", url_, e.what());
    token->Release();
  } catch (...) {
    MW_WARN("shm retained callback for '{}' threw an unknown exception.", url_);
    token->Release();
  }
}

void ShmSubscription::ReleaseSample(const void* payload, bool leased) noexcept {
  subscriber_.Release(payload);
  if (!leased) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (starved_ || stopping_) {
      // Arrived over the lease limit: dropped, never replayed, the same as a
      // sample that finds the spare slot of a smaller max_outstanding.
      const bool count = starved_ && !stopping_;
      starved_ = false;
      while (auto t = subscriber_.Take()) {
        subscriber_.Release(t.value());
        if (count) dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    --outstanding_;
  }
  cv_.notify_all();
}

void ShmSubscription::DiscardQueued() noexcept {
  while (auto t = subscriber_.Take()) subscriber_.Release(t.value());
}

void ShmSubscription::Stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  DiscardQueued();
}

bool ShmSubscription::Stopping() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return stopping_;
}

size_t ShmSubscription::Outstanding() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return outstanding_;
}

void ShmSubscription::Finalize() noexcept {
  std::shared_ptr<shm::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(attach_mu_);
    attach_closed_ = true;
    listener = std::move(listener_);
  }
  if (listener) listener->Detach(subscriber_);
  DiscardQueued();
  if (!finalized_.exchange(true) && registry_) registry_->Remove(url_, this);
}

bool ShmSubscription::Close(std::chrono::milliseconds timeout) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Stop();
  if (t_current_subscription == this) return false;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (!WaitWithDeadline(cv_, lock, deadline, [this] { return QuiescentLocked(); })) return false;
  }
  Finalize();
  return true;
}

bool ShmSubscription::TryFinalize() noexcept {
  if (t_current_subscription == this) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!stopping_ || !QuiescentLocked()) return false;
  }
  Finalize();
  return true;
}

// ============================================================ listener pool

void ShmListenerPool::SetThreadName(std::string thread_name) {
  std::lock_guard<std::mutex> lock(mu_);
  thread_name_ = std::move(thread_name);
}

void ShmListenerPool::SetThreadSetup(std::function<void()> setup) {
  std::lock_guard<std::mutex> lock(mu_);
  thread_setup_ = setup ? std::make_shared<const std::function<void()>>(std::move(setup)) : nullptr;
}

bool ShmListenerPool::Attach(const std::shared_ptr<ShmSubscription>& subscription) {
  std::lock_guard<std::mutex> lock(mu_);
  // attach_mu_ is held across the listener's Attach so Finalize() either sees
  // the listener to detach from or stops this attach.
  std::lock_guard<std::mutex> attach_lock(subscription->attach_mu_);
  if (subscription->listener_ || subscription->attach_closed_ || subscription->Stopping()) return true;
  // Written before the listener's lock publishes the subscription to its thread.
  subscription->thread_setup_ = thread_setup_;
  auto try_attach = [&](const std::shared_ptr<shm::Listener>& l) {
    if (!l->Attach(subscription->subscriber_, &ShmSubscription::OnEvent, subscription.get())) return false;
    subscription->listener_ = l;
    return true;
  };
  for (auto& l : listeners_) {
    if (try_attach(l)) return true;
  }
  shm::ListenerOptions lo;
  lo.thread_name = thread_name_;
  auto l = std::make_shared<shm::Listener>(lo);
  if (!l->Valid()) {
    MW_ERROR("Cannot start a shm listener thread: {}", shm::ErrorName(l->InitError()));
    return false;
  }
  listeners_.push_back(l);
  return try_attach(l);
}

size_t ShmListenerPool::Threads() const {
  std::lock_guard<std::mutex> lock(mu_);
  return listeners_.size();
}

void ShmListenerPool::Clear() noexcept {
  std::vector<std::shared_ptr<shm::Listener>> listeners;
  {
    std::lock_guard<std::mutex> lock(mu_);
    listeners.swap(listeners_);
  }
  listeners.clear();  // joins the threads outside the lock
}

void ShmListenerPool::Abandon() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  new std::vector<std::shared_ptr<shm::Listener>>(std::move(listeners_));  // NOLINT: leaked on purpose
  listeners_.clear();
}

// ============================================================ bus

ShmBus::ShmBus() : registry_(std::make_shared<ShmSubscriptionRegistry>()), listeners_("ibmw_shm_lsn") {}

ShmBus::~ShmBus() {
  if (!initialized_ || shut_down_) return;
  if (TryPrepareShutdown(std::chrono::seconds(1))) {
    Shutdown();
    return;
  }
  // A publish is still blocked, or loans/retained samples are outstanding:
  // freeing now would turn a stuck shutdown into use-after-free.
  MW_ERROR("ShmBus destroyed while still in use; leaking its publishers, subscriptions and listener threads.");
  {
    std::unique_lock<std::shared_mutex> lock(pub_mu_);
    new decltype(publishers_)(std::move(publishers_));  // NOLINT: leaked on purpose
    publishers_.clear();
  }
  listeners_.Abandon();
  new std::shared_ptr<ShmSubscriptionRegistry>(registry_);  // NOLINT: leaked on purpose
  (void)gate_.release();
}

void ShmBus::Initialize(const ShmBusOptions& options) {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  if (initialized_ && !shut_down_) throw std::runtime_error("shm bus is already initialized");
  const auto init_size = NextShmLoanSize(options.shm_init_size, options.shm_init_size);
  if (!init_size) throw std::runtime_error("shm_init_size exceeds the largest shm payload");
  shm::RuntimeOptions ro;
  ro.name = options.runtime_name;
  if (!options.domain.empty()) ro.domain = options.domain;
  ro.root_dir = options.root_dir;
  ro.group = options.group;
  if (auto r = shm::Runtime::Init(ro); !r) {
    if (r.error() == shm::Error::kAlreadyInitialized) {
      throw std::runtime_error("shm runtime already runs in '" + shm::Runtime::ServiceDirectory() +
                               "'; a process supports one shm domain/root/group, requested domain '" + options.domain +
                               "', root '" + options.root_dir + "', group '" +
                               (options.group ? std::to_string(*options.group) : std::string()) + "'");
    }
    if (r.error() == shm::Error::kInvalidArg) {
      throw std::runtime_error(
          "shm runtime configuration is invalid: root_dir/IBMW_SHM_ROOT is required on OHOS and must leave the "
          "service directory within the socket path limit, domain/IBMW_SHM_DOMAIN must be at most 32 characters "
          "without '/', IBMW_SHM_GROUP must be a numeric gid (requested domain '" + options.domain + "', root '" +
          options.root_dir + "', group '" + (options.group ? std::to_string(*options.group) : std::string()) + "')");
    }
    throw std::runtime_error(std::string("shm runtime initialisation failed: ") + shm::ErrorName(r.error()));
  }
  // Nothing above changed the bus: a failed re-Initialize leaves it shut down.
  if (shut_down_ || prepared_ || !gate_->Accepting()) ResetForNewGenerationLocked();
  options_ = options;
  options_.shm_init_size = *init_size;
  listeners_.SetThreadName(options_.listener_thread_name);
  initialized_ = true;
}

void ShmBus::ResetForNewGenerationLocked() {
  prepared_ = false;
  shut_down_ = false;
  delivery_ = {};
  {
    std::unique_lock<std::shared_mutex> lock(pub_mu_);
    pub_closed_ = false;
  }
  {
    std::lock_guard<std::mutex> lock(registry_->mu);
    registry_->shutting_down = false;
    registry_->delivery_started = false;
  }
  listeners_.SetThreadSetup(nullptr);
  generation_.fetch_add(1, std::memory_order_release);
  gate_->Reopen();  // last: a lease from here on sees all of the above
}

void ShmBus::SetDeliveryOptions(const ShmDeliveryOptions& options) noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  delivery_ = options;
}

ShmDeliveryOptions ShmBus::DeliveryOptions() const noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  return delivery_;
}

void ShmBus::SetListenerThreadName(std::string name) { listeners_.SetThreadName(std::move(name)); }

void ShmBus::SetListenerThreadSetup(std::function<void()> setup) { listeners_.SetThreadSetup(std::move(setup)); }

ShmPublisher* ShmBus::RegisterPublisher(std::string_view url, size_t size_hint, bool* created) {
  auto operation = TryAcquireOperation();
  if (!operation) throw std::runtime_error("shm bus is shutting down");
  CheckUrlLength(url);
  ShmDeliveryOptions delivery;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (!initialized_) throw std::runtime_error("shm bus is not initialized");
    delivery = delivery_;
  }
  std::unique_lock<std::shared_mutex> lock(pub_mu_);
  // TryPrepareShutdown closes the gate before it snapshots the publishers
  // under pub_mu_: a publisher added after the close would miss
  // BeginShutdown, so none is.
  if (pub_closed_ || !gate_->Accepting()) throw std::runtime_error("shm bus is shutting down");
  if (auto it = publishers_.find(url); it != publishers_.end()) {
    if (created) *created = false;
    return it->second.get();
  }
  const auto hint = NextShmLoanSize(size_hint == 0 ? options_.shm_init_size : size_hint, 1);
  if (!hint) throw std::runtime_error("shm publisher size hint exceeds the largest shm payload");
  auto publisher = std::make_unique<ShmPublisher>(std::string(url), options_, delivery, *hint);
  if (!publisher->Valid()) {
    throw std::runtime_error(std::string("cannot create shm publisher for '") + std::string(url) +
                             "': " + shm::ErrorName(publisher->InitError()));
  }
  auto* result = publisher.get();
  publishers_.emplace(std::string(url), std::move(publisher));
  if (created) *created = true;
  return result;
}

ShmPublisher* ShmBus::GetPublisher(const ShmOperationGate::Lease& lease, std::string_view url) const {
  if (!lease) return nullptr;
  std::shared_lock<std::shared_mutex> lock(pub_mu_);
  if (pub_closed_) return nullptr;
  auto it = publishers_.find(url);
  return it != publishers_.end() ? it->second.get() : nullptr;
}

void ShmBus::RollbackPublisher(std::string_view url, const ShmPublisher* expected) noexcept {
  std::unique_ptr<ShmPublisher> victim;
  {
    std::unique_lock<std::shared_mutex> lock(pub_mu_);
    auto it = publishers_.find(url);
    if (it == publishers_.end() || it->second.get() != expected) return;
    victim = std::move(it->second);
    publishers_.erase(it);
  }
}

bool ShmBus::ReclaimStoppedLocked(std::string_view url) noexcept {
  auto it = registry_->subscriptions.find(url);
  if (it == registry_->subscriptions.end()) return true;
  auto previous = it->second;
  if (!previous->Stopping()) return false;
  // Finalize() takes the registry lock itself.
  registry_->mu.unlock();
  const bool done = previous->TryFinalize();
  registry_->mu.lock();
  return done && registry_->subscriptions.find(url) == registry_->subscriptions.end();
}

bool ShmBus::ReserveSubscriberUrl(std::string_view url) {
  auto operation = TryAcquireOperation();
  if (!operation) return false;
  std::lock_guard<std::mutex> lock(registry_->mu);
  if (registry_->shutting_down || registry_->reserved.count(url) != 0) return false;
  if (!ReclaimStoppedLocked(url)) return false;
  if (registry_->shutting_down || registry_->reserved.count(url) != 0) return false;
  registry_->reserved.emplace(url);
  return true;
}

void ShmBus::RollbackSubscriberUrl(std::string_view url) noexcept {
  std::lock_guard<std::mutex> lock(registry_->mu);
  if (auto it = registry_->reserved.find(url); it != registry_->reserved.end()) registry_->reserved.erase(it);
}

std::shared_ptr<ShmSubscription> ShmBus::RegisterSubscriber(std::string_view url, ShmMessageHandler handler,
                                                            ShmSubscriptionOptions options) {
  auto operation = TryAcquireOperation();
  if (!operation) throw std::runtime_error("shm bus is shutting down");
  CheckUrlLength(url);
  if (options.retained && (options.max_outstanding == 0 || options.max_outstanding > shm::kMaxChunksHeldPerSubscriber)) {
    throw std::runtime_error("shm retained max_outstanding must be 1..256");
  }
  ShmDeliveryOptions delivery;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (!initialized_) throw std::runtime_error("shm bus is not initialized");
    delivery = delivery_;
  }
  {
    std::lock_guard<std::mutex> lock(registry_->mu);
    if (registry_->shutting_down) throw std::runtime_error("shm bus is shutting down");
    if (options.url_reserved) {
      if (registry_->reserved.count(url) == 0) throw std::runtime_error("shm subscriber URL was not reserved");
    } else {
      if (registry_->reserved.count(url) != 0 || !ReclaimStoppedLocked(url) || registry_->reserved.count(url) != 0) {
        throw std::runtime_error("shm subscriber URL is already registered");
      }
      registry_->reserved.emplace(url);
    }
  }
  bool attach_now = false;
  std::shared_ptr<ShmSubscription> subscription;
  try {
    subscription = std::make_shared<ShmSubscription>(std::string(url), std::move(handler), options, delivery,
                                                     registry_);
    if (!subscription->Valid()) {
      throw std::runtime_error(std::string("cannot create shm subscriber for '") + std::string(url) +
                               "': " + shm::ErrorName(subscription->InitError()));
    }
    {
      std::lock_guard<std::mutex> lock(registry_->mu);
      if (registry_->shutting_down) throw std::runtime_error("shm bus is shutting down");
      registry_->reserved.erase(registry_->reserved.find(url));
      registry_->subscriptions.emplace(std::string(url), subscription);
      attach_now = registry_->delivery_started;
    }
  } catch (...) {
    RollbackSubscriberUrl(url);
    throw;
  }
  // Attached last: no callback can run before the subscription is
  // registered, so Close()/shutdown always see it. Decided under the registry
  // lock, so StartDelivery()'s snapshot has it whenever this skips the attach.
  if (attach_now && !listeners_.Attach(subscription)) {
    subscription->Stop();
    subscription->TryFinalize();
    throw std::runtime_error("cannot attach shm subscriber to a listener thread");
  }
  return subscription;
}

void ShmBus::StartDelivery() {
  auto operation = TryAcquireOperation();
  if (!operation) return;
  std::vector<std::shared_ptr<ShmSubscription>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(registry_->mu);
    if (registry_->shutting_down) return;
    registry_->delivery_started = true;
    for (auto& [url, s] : registry_->subscriptions) subscriptions.push_back(s);
  }
  for (auto& s : subscriptions) {
    if (!listeners_.Attach(s)) throw std::runtime_error("cannot attach shm subscriber to a listener thread");
  }
}

bool ShmBus::TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept {
  if (IsShmCallbackThread()) return false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (prepared_ || shut_down_) return true;
  }
  gate_->Close();
  std::vector<std::shared_ptr<ShmSubscription>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(registry_->mu);
    registry_->shutting_down = true;
    for (auto& [url, s] : registry_->subscriptions) subscriptions.push_back(s);
  }
  std::vector<ShmPublisher*> publishers;
  {
    std::shared_lock<std::shared_mutex> lock(pub_mu_);
    for (auto& [url, p] : publishers_) {
      p->BeginShutdown();
      publishers.push_back(p.get());
    }
  }
  for (auto& s : subscriptions) s->Stop();
  for (auto& s : subscriptions) {
    if (!s->Close(Remaining(deadline))) {
      MW_WARN("Timed out preparing shm subscription '{}' for shutdown ({} samples still held).", s->Url(),
              s->Outstanding());
      return false;
    }
  }
  if (!gate_->WaitIdle(deadline)) {
    MW_WARN("Timed out waiting for active shm operations during shutdown preparation.");
    return false;
  }
  for (auto* p : publishers) {
    if (!p->WaitForLoans(deadline)) {
      MW_WARN("Timed out preparing shm publisher '{}' with {} outstanding loans.", p->Url(), p->Outstanding());
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  prepared_ = true;
  return true;
}

bool ShmBus::Prepared() const noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  return prepared_;
}

bool ShmBus::Shutdown() noexcept {
  if (IsShmCallbackThread()) {
    MW_ERROR("ShmBus::Shutdown cannot run on a shm listener callback thread.");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (shut_down_) return true;
    if (!prepared_) {
      MW_ERROR("ShmBus::Shutdown requires a successful TryPrepareShutdown; resources stay intact.");
      return false;
    }
  }
  std::unordered_map<std::string, std::shared_ptr<ShmSubscription>, ibmw::common::util::StringHash, std::equal_to<>>
      subscriptions;
  {
    std::lock_guard<std::mutex> lock(registry_->mu);
    subscriptions.swap(registry_->subscriptions);
    registry_->reserved.clear();
  }
  decltype(publishers_) publishers;
  {
    std::unique_lock<std::shared_mutex> lock(pub_mu_);
    pub_closed_ = true;
    publishers.swap(publishers_);
  }
  subscriptions.clear();
  listeners_.Clear();
  publishers.clear();
  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  shut_down_ = true;
  return true;
}

size_t ShmBus::PublisherCount() const {
  std::shared_lock<std::shared_mutex> lock(pub_mu_);
  return publishers_.size();
}

size_t ShmBus::SubscriptionCount() const {
  std::lock_guard<std::mutex> lock(registry_->mu);
  return registry_->subscriptions.size();
}

ShmBusStats ShmBus::Stats() const {
  ShmBusStats total;
  {
    std::shared_lock<std::shared_mutex> lock(pub_mu_);
    for (const auto& [url, p] : publishers_) {
      const auto s = p->Stats();
      total.quarantined += s.quarantined;
      total.quarantined_chunks += s.quarantined_chunks;
      total.unverifiable_peers += s.unverifiable_peers;
      total.unsealed_segments += s.unsealed_segments;
      total.unknown_messages += s.unknown_messages;
      total.deferred_sends += s.deferred_sends;
      total.send_queue_overflows += s.send_queue_overflows;
    }
  }
  std::lock_guard<std::mutex> lock(registry_->mu);
  for (const auto& [url, sub] : registry_->subscriptions) {
    const auto s = sub->Stats();
    total.unknown_messages += s.unknown_messages;
    total.deferred_sends += s.deferred_sends;
    total.send_queue_overflows += s.send_queue_overflows;
    total.connect_retries += s.connect_retries;
  }
  return total;
}

}  // namespace ibmw::extensions::shm_extension
