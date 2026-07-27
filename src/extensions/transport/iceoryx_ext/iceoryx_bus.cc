// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_bus.h"
#include <algorithm>
#include "iceoryx_ext/global.h"
#include "iceoryx_posh/mepoo/chunk_header.hpp"
#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_versions.hpp"

namespace ibmw::extensions::iceoryx_extension {

namespace {
thread_local IoxSubscriptionState* current_callback_subscription = nullptr;

#if ICEORYX_VERSION_MAJOR == 2 && ICEORYX_VERSION_MINOR < 90
using IoxTruncateToCapacity = iox::cxx::TruncateToCapacity_t;
constexpr auto kIoxTruncateToCapacity = iox::cxx::TruncateToCapacity;
#else
using IoxTruncateToCapacity = iox::TruncateToCapacity_t;
constexpr auto kIoxTruncateToCapacity = iox::TruncateToCapacity;
#endif
}

bool IsIceoryxListenerCallbackThread() noexcept {
  return current_callback_subscription != nullptr;
}

bool IoxNotificationGate::TryEnter() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return false;
  ++active_;
  return true;
}

void IoxNotificationGate::Exit() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ != 0) --active_;
  }
  cv_.notify_all();
}

void IoxNotificationGate::Close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
}

void IoxNotificationGate::WaitForQuiescence() noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return active_ == 0; });
}

size_t IoxNotificationGate::Active() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

bool IoxSubscriptionRegistry::Reserve(std::string_view url) {
  if (subscriptions_.contains(url) || reservations_.contains(url)) return false;
  return reservations_.emplace(url).second;
}

bool IoxSubscriptionRegistry::Commit(
    std::string_view url, std::shared_ptr<IoxSubscriptionState> state) {
  const auto reservation = reservations_.find(url);
  if (reservation == reservations_.end() || subscriptions_.contains(url)) {
    return false;
  }
  auto key = *reservation;
  reservations_.erase(reservation);
  return subscriptions_.emplace(std::move(key), std::move(state)).second;
}

void IoxSubscriptionRegistry::Rollback(std::string_view url) noexcept {
  const auto it = reservations_.find(url);
  if (it != reservations_.end()) reservations_.erase(it);
}

void IoxSubscriptionRegistry::Remove(
    std::string_view url, const IoxSubscriptionState* expected) noexcept {
  const auto it = subscriptions_.find(url);
  if (it != subscriptions_.end() && it->second.get() == expected) {
    subscriptions_.erase(it);
  }
}

bool IoxSubscriptionRegistry::Contains(std::string_view url) const {
  return subscriptions_.contains(url) || reservations_.contains(url);
}

using IdString_t = iox::capro::IdString_t;

// iceoryx rules each part of the service name should be less than 100 characters :IdString_t = cxx::string<100>;
std::string TruncateString(std::string_view input) {
  if (input.length() <= iox::MAX_RUNTIME_NAME_LENGTH) [[likely]] {
    return std::string(input);
  }

  size_t ellipsis_length = 3;
  size_t prefix_length = (iox::MAX_RUNTIME_NAME_LENGTH - ellipsis_length) / 2;
  size_t suffix_length = iox::MAX_RUNTIME_NAME_LENGTH - ellipsis_length - prefix_length;

  std::string truncated_string;
  truncated_string.reserve(iox::MAX_RUNTIME_NAME_LENGTH);
  truncated_string.append(input.substr(0, prefix_length));
  truncated_string.append("...");
  truncated_string.append(input.substr(input.length() - suffix_length));

  MW_WARN("Input url is too long. Each part should be less than {} characters. The input :{} has been truncated to :{}, which may lead to potential risks.",
             iox::MAX_RUNTIME_NAME_LENGTH, input, truncated_string);

  return truncated_string;
}

// iceoryx uses the iox::cxx::TruncateToCapacity_t to limit the length of the string to 100 characters
IdString_t String2IdString(std::string_view str) {
  std::string truncated_str = TruncateString(str);
  IoxTruncateToCapacity truncate_to_capacity;
  return {truncate_to_capacity, truncated_str.c_str(), truncated_str.length()};
}

// The iox description has three parts: "service name", "instance", "specific object"
iox::capro::ServiceDescription Url2ServiceDescription(std::string_view url) {
  size_t first_slash_pos = url.find('/');
  size_t second_slash_pos = url.find('/', first_slash_pos + 1);
  size_t third_slash_pos = url.find('/', second_slash_pos + 1);

  return iox::capro::ServiceDescription{
      String2IdString(url.substr(0, second_slash_pos - 0)),
      String2IdString(url.substr(second_slash_pos, third_slash_pos - second_slash_pos)),
      String2IdString(url.substr(third_slash_pos))};
}

IoxPublisher::IoxPublisher(std::string_view url, size_t shm_size)
    : shm_size_(NextIoxShmSize(shm_size, shm_size).value()),
      publisher_(Url2ServiceDescription(url)) {}

void IoxLoanTracker::BeginShutdown() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  closing_ = true;
}

bool IoxLoanTracker::WaitForLoans(
    std::chrono::milliseconds timeout) noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout.count() <= 0) return outstanding_ == 0;
  return cv_.wait_for(lock, timeout, [this] { return outstanding_ == 0; });
}

bool IoxLoanTracker::IsClosing() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return closing_;
}

size_t IoxLoanTracker::Outstanding() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_;
}

IoxLoanedShm IoxPublisher::LoanShm(size_t min_size) {
  void* chunk = nullptr;
  size_t loan_size = 0;
  const bool loaned = loan_tracker_.TryTrackLoan([&] {
    const auto next_size = NextIoxShmSize(shm_size_, min_size);
    if (!next_size) return false;
    shm_size_ = *next_size;
    auto loan_result = publisher_.loan(shm_size_);
    if (loan_result.has_error()) return false;
    chunk = loan_result.value();
    loan_size = shm_size_;
    return true;
  });
  if (!loaned) {
    throw std::runtime_error(loan_tracker_.IsClosing()
                                 ? "Iceoryx publisher is shutting down"
                                 : "Failed to loan shm");
  }

  return IoxLoanedShm(chunk, loan_size, this);
}

void IoxPublisher::ReleaseShm(IoxLoanedShm& loaned_shm) noexcept {
  if (loaned_shm.ptr_) {
    loan_tracker_.CompleteTrackedLoan(
        [this, &loaned_shm] { publisher_.release(loaned_shm.ptr_); });
    loaned_shm.ptr_ = nullptr;
  }
}

void IoxPublisher::UpdateLoanShm(IoxLoanedShm& loaned_shm, size_t min_size) {
  const bool replaced = loan_tracker_.ReplaceTrackedLoan([&] {
    const auto next_size = NextIoxShmSize(shm_size_, min_size);
    if (!next_size) return false;
    auto loan_result = publisher_.loan(*next_size);
    if (loan_result.has_error()) return false;
    void* const old_chunk = loaned_shm.ptr_;
    void* const new_chunk = loan_result.value();
    if (old_chunk) publisher_.release(old_chunk);
    shm_size_ = *next_size;
    loaned_shm.ptr_ = new_chunk;
    loaned_shm.size_ = *next_size;
    return true;
  });
  if (!replaced) throw std::runtime_error("Failed to replace loaned shm");
}

void IoxPublisher::SendShm(IoxLoanedShm& loaned_shm) {
  if (!loaned_shm.ptr_) return;
  loan_tracker_.CompleteTrackedLoan(
      [this, &loaned_shm] { publisher_.publish(loaned_shm.ptr_); });
  loaned_shm.ptr_ = nullptr;
}

// ---------- Zero-copy POD-only path ----------
//
// LoanRaw is similar to LoanShm but does not wrap the pointer in IoxLoanedShm.
// Caller is responsible for passing the same pointer back via PublishRaw or
// ReleaseRaw. This API exists so the core LoanedSampleHandle (which already
// carries its own driver_token) does not need to nest an RAII helper.
std::optional<IoxRawLoanRequest> MakeIoxRawLoanRequest(
    size_t requested_size, size_t alignment) noexcept {
  if (requested_size > std::numeric_limits<uint32_t>::max() ||
      alignment == 0 || alignment > std::numeric_limits<uint32_t>::max() ||
      (alignment & (alignment - 1)) != 0) return std::nullopt;
  return IoxRawLoanRequest{static_cast<uint32_t>(requested_size),
                           static_cast<uint32_t>(alignment)};
}

std::optional<size_t> NextIoxShmSize(size_t current_size,
                                    size_t min_size) noexcept {
  constexpr size_t kMaxPayload = std::numeric_limits<uint32_t>::max();
  if (min_size > kMaxPayload) return std::nullopt;
  size_t size = current_size == 0 ? 1U : current_size;
  if (size > kMaxPayload) return std::nullopt;
  while (size < min_size) {
    if (size > kMaxPayload / 2U) {
      size = kMaxPayload;
      break;
    }
    size *= 2U;
  }
  return size >= min_size ? std::optional<size_t>(size) : std::nullopt;
}

void* IoxPublisher::LoanRaw(size_t requested_size, size_t alignment) {
  const auto request = MakeIoxRawLoanRequest(requested_size, alignment);
  if (!request) return nullptr;
  void* chunk = nullptr;
  if (!loan_tracker_.TryTrackLoan([&] {
        auto loan_result =
            publisher_.loan(request->payload_size, request->alignment);
        if (loan_result.has_error()) return false;
        chunk = loan_result.value();
        return true;
      })) {
    return nullptr;
  }
  return chunk;
}

void IoxPublisher::PublishRaw(void* chunk_ptr) noexcept {
  if (chunk_ptr == nullptr) return;
  loan_tracker_.CompleteTrackedLoan(
      [this, chunk_ptr] { publisher_.publish(chunk_ptr); });
}

void IoxPublisher::ReleaseRaw(void* chunk_ptr) noexcept {
  if (chunk_ptr == nullptr) return;
  loan_tracker_.CompleteTrackedLoan(
      [this, chunk_ptr] { publisher_.release(chunk_ptr); });
}

iox::popo::SubscriberOptions MakeSubscriberOptions(bool retained) noexcept {
  iox::popo::SubscriberOptions options;
  if (retained) {
    // Retained delivery is deliberately bounded: once all leases are held,
    // incoming samples are dropped and are never replayed after a release.
    options.queueCapacity = 1;
    options.queueFullPolicy = iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA;
    options.historyRequest = 0;
  }
  return options;
}

struct IoxAsyncStop::State {
  explicit State(std::function<void()> stop_action)
      : completion_future(completion_promise.get_future().share()),
        action(std::move(stop_action)) {}

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::promise<void> completion_promise;
  std::shared_future<void> completion_future;
  std::function<void()> action;
  bool requested = false;
  bool shutdown = false;
  std::shared_ptr<void> keepalive;
};

IoxAsyncStop::IoxAsyncStop(std::function<void()> action)
    : state_(std::make_shared<State>(std::move(action))),
      worker_([state = state_] { Run(std::move(state)); }) {}

IoxAsyncStop::~IoxAsyncStop() {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->shutdown = true;
  }
  state_->cv.notify_all();
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id()) {
      worker_.detach();
    } else {
      worker_.join();
    }
  }
}

void IoxAsyncStop::Request(std::shared_ptr<void> keepalive) noexcept {
  const auto state = state_;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->requested) return;
    state->keepalive = std::move(keepalive);
    state->requested = true;
  }
  state->cv.notify_all();
}

void IoxAsyncStop::Run(std::shared_ptr<State> state) noexcept {
  std::shared_ptr<void> keepalive;
  std::unique_lock<std::mutex> lock(state->mutex);
  state->cv.wait(lock,
                 [&state] { return state->requested || state->shutdown; });
  if (!state->requested) return;
  keepalive = std::move(state->keepalive);
  lock.unlock();
  try {
    if (state->action) state->action();
  } catch (...) {
  }
  try {
    state->completion_promise.set_value();
  } catch (...) {
  }
}

bool IoxAsyncStop::Close(std::chrono::milliseconds timeout,
                          bool called_from_callback) noexcept {
  if (called_from_callback) return false;
  const auto wait_timeout = std::max(timeout, std::chrono::milliseconds(0));
  return state_->completion_future.wait_for(wait_timeout) ==
         std::future_status::ready;
}

bool IoxAsyncStop::Complete() const noexcept {
  return state_->completion_future.wait_for(std::chrono::milliseconds(0)) ==
         std::future_status::ready;
}

IceoryxBus::IceoryxBus() : control_(std::make_shared<IoxBusControl>()) {}

bool IoxCallbackContextRegistry::Remove(
    const IoxSubscriptionCallbackContext* expected) noexcept {
  const auto it = std::find_if(
      contexts_.begin(), contexts_.end(),
      [expected](const auto& context) { return context.get() == expected; });
  if (it == contexts_.end()) return false;
  contexts_.erase(it);
  return true;
}

IceoryxBus::~IceoryxBus() {
  if (!TryPrepareShutdown(std::chrono::seconds(1))) {
    MW_FATAL("IceoryxBus destruction could not quiesce callbacks/loans. The "
             "runtime must outlive every external raw loan handle; C++ member "
             "destruction cannot protect handles that violate that lifetime "
             "precondition.");
    return;
  }
  Shutdown();
}

void IceoryxBus::Initialize(uint64_t shm_init_size, std::string_view runtime_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  prepared_ = false;
  if (shm_init_size > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Iceoryx initial shm size exceeds payload limit");
  }
  const auto normalized_size = NextIoxShmSize(
      static_cast<size_t>(shm_init_size), static_cast<size_t>(shm_init_size));
  if (!normalized_size) {
    throw std::runtime_error("Iceoryx initial shm size exceeds payload limit");
  }
  shm_init_size_ = *normalized_size;
  const std::string runtime_name(runtime_id);
  iox::runtime::PoshRuntime::initRuntime(
      iox::RuntimeName_t(kIoxTruncateToCapacity, runtime_name.c_str()));

  iox_listener_ptr_ = std::make_shared<iox::popo::Listener>();
  std::lock_guard<std::mutex> control_lock(control_->mutex);
  control_->shutting_down = false;
  control_->listener = iox_listener_ptr_;
}

bool IceoryxBus::TryPrepareShutdown(
    std::chrono::milliseconds timeout) noexcept {
  if (IsIceoryxListenerCallbackThread()) {
    return false;
  }

  try {
    const auto shutdown_deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<
        std::pair<std::string, std::shared_ptr<IoxSubscriptionState>>>
        states;
    std::vector<std::pair<std::string, IoxPublisher*>> publishers;
    std::shared_ptr<IoxBusControl> control;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (prepared_) return true;
      control = control_;
    }
    {
      std::unique_lock<std::mutex> control_lock(control->mutex);
      control->shutting_down = true;
      if (!control->registration_cv.wait_until(
              control_lock, shutdown_deadline, [&control] {
                return control->registrations_in_flight == 0;
              })) {
        MW_WARN("Timed out waiting for iceoryx subscriber registration to "
                "finish during shutdown preparation.");
        return false;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (prepared_) return true;
      std::lock_guard<std::mutex> control_lock(control->mutex);
      // Stop intake and block every publisher before any snapshot allocation
      // can fail. A later retry can rebuild the snapshots and continue waiting.
      for (auto& entry : control->subscriptions.Entries()) entry.second->Stop();
      for (auto& entry : iox_pub_catalog_) entry.second->BeginShutdown();

      states.reserve(control->subscriptions.Size());
      for (auto& entry : control->subscriptions.Entries()) {
        states.emplace_back(entry.first, entry.second);
      }
      publishers.reserve(iox_pub_catalog_.size());
      for (auto& entry : iox_pub_catalog_) {
        publishers.emplace_back(entry.first, entry.second.get());
      }
    }
    for (auto& [url, subscription] : states) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              shutdown_deadline - std::chrono::steady_clock::now());
      if (!subscription->Close(
              std::max(remaining, std::chrono::milliseconds(0)))) {
        MW_WARN("Timed out preparing iceoryx subscription '{}' for shutdown.",
                url);
        return false;
      }
    }
    for (auto& [url, publisher] : publishers) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              shutdown_deadline - std::chrono::steady_clock::now());
      if (!publisher->WaitForLoans(
              std::max(remaining, std::chrono::milliseconds(0)))) {
        MW_WARN("Timed out preparing iceoryx publisher '{}' with outstanding "
                "loans.", url);
        return false;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      prepared_ = true;
    }
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("Iceoryx shutdown preparation failed without finalizing "
             "resources: {}", e.what());
  } catch (...) {
    MW_ERROR("Iceoryx shutdown preparation failed with an unknown error; "
             "resources remain intact for retry.");
  }
  return false;
}

void IceoryxBus::Shutdown() {
  if (IsIceoryxListenerCallbackThread()) {
    MW_ERROR("IceoryxBus::Shutdown cannot run on an iceoryx listener callback thread; runtime lifecycle shutdown must run on its owner thread.");
    return;
  }
  decltype(control_->subscriptions.ExtractAll()) subscriptions;
  decltype(iox_pub_catalog_) publishers;
  std::vector<std::shared_ptr<IoxSubscriptionCallbackContext>>
      callback_contexts;
  std::shared_ptr<iox::popo::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!prepared_) {
      MW_FATAL("IceoryxBus::Shutdown requires successful shutdown preparation; "
               "resources were not finalized.");
      return;
    }
    std::lock_guard<std::mutex> control_lock(control_->mutex);
    listener = std::move(control_->listener);
    subscriptions = control_->subscriptions.ExtractAll();
    callback_contexts = control_->callback_contexts.ExtractAll();
    iox_listener_ptr_.reset();
    publishers.swap(iox_pub_catalog_);
  }
  // Destroy states/publishers only after both control locks are released.
  subscriptions.clear();
  listener.reset();
  callback_contexts.clear();
  publishers.clear();
}

void IceoryxBus::RegisterPublisher(std::string_view url,
                                   size_t initial_size_hint) {
  std::lock_guard<std::mutex> lock(mutex_);
  {
    std::lock_guard<std::mutex> control_lock(control_->mutex);
    if (control_->shutting_down || iox_listener_ptr_ == nullptr) {
      throw std::runtime_error("Iceoryx bus is shutting down");
    }
  }
  const size_t requested_size =
      initial_size_hint == 0 ? shm_init_size_ : initial_size_hint;
  const auto initial_size = NextIoxShmSize(requested_size, requested_size);
  if (!initial_size) {
    throw std::runtime_error("Iceoryx publisher shm size exceeds payload limit");
  }
  iox_pub_catalog_.emplace(
      url, std::make_unique<IoxPublisher>(url, *initial_size));
}

void OnReceivedCallback(iox::popo::UntypedSubscriber*,
                         IoxSubscriptionCallbackContext* context) {
  context->Invoke();
}

std::shared_ptr<IoxSubscriptionState> IceoryxBus::RegisterSubscriber(
    std::string_view url,
    MsgHandleFunc&& handle,
    IoxSubscriberRegistrationOptions options) {
  std::shared_ptr<IoxBusControl> control;
  std::shared_ptr<iox::popo::Listener> listener;
  std::optional<IoxRegistrationGuard> registration;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    control = control_;
    registration.emplace(control);
    std::lock_guard<std::mutex> control_lock(control->mutex);
    if (control->shutting_down) {
      throw std::runtime_error("Iceoryx bus is shutting down");
    }
    if (!control->listener) {
      throw std::runtime_error("Iceoryx bus is not initialized");
    }
    if (!control->subscriptions.Reserve(url)) {
      throw std::runtime_error("Iceoryx subscriber URL is already registered");
    }
    registration->StartLocked();
    listener = control->listener;
  }

  std::shared_ptr<IoxSubscriptionState> subscription;
  std::shared_ptr<IoxSubscriptionCallbackContext> callback_context;
  bool attached = false;
  try {
    subscription = std::make_shared<IoxSubscriptionState>(
        url, std::move(handle), options, control);
    callback_context =
        std::make_shared<IoxSubscriptionCallbackContext>(subscription);
    subscription->SetCallbackContext(callback_context);
    {
      std::lock_guard<std::mutex> control_lock(control->mutex);
      control->callback_contexts.Add(callback_context);
    }
    listener->attachEvent(
                       subscription->Subscriber(),
                       iox::popo::SubscriberEvent::DATA_RECEIVED,
                       iox::popo::createNotificationCallback<
                           iox::popo::UntypedSubscriber,
                           IoxSubscriptionCallbackContext>(OnReceivedCallback,
                                                           *callback_context))
        .or_else([](auto) {
          throw std::runtime_error("Unable to attach a subscriber!");
        });
    attached = true;

    std::lock_guard<std::mutex> control_lock(control->mutex);
    if (control->shutting_down ||
        !control->subscriptions.Commit(url, subscription)) {
      throw std::runtime_error("Iceoryx subscriber URL is already registered");
    }
  } catch (...) {
    internal::RunIoxRegistrationRollback(
        [&] {
          if (callback_context) callback_context->Close();
        },
        [&] {
          if (attached && subscription) {
            listener->detachEvent(
                subscription->Subscriber(),
                iox::popo::SubscriberEvent::DATA_RECEIVED);
          }
        },
        [&] {
          if (callback_context) callback_context->WaitForQuiescence();
        },
        [&] {
          std::lock_guard<std::mutex> control_lock(control->mutex);
          control->subscriptions.Rollback(url);
          if (callback_context) {
            control->callback_contexts.Remove(callback_context.get());
          }
        });
    throw;
  }
  return subscription;
}

IoxPublisher* IceoryxBus::GetPublisher(std::string_view url) {
  std::lock_guard<std::mutex> lock(mutex_);
  {
    std::lock_guard<std::mutex> control_lock(control_->mutex);
    if (control_->shutting_down) return nullptr;
  }
  auto it = iox_pub_catalog_.find(url);
  if (it != iox_pub_catalog_.end())
    return it->second.get();
  return nullptr;
}

IoxSubscriptionState::IoxSubscriptionState(
    std::string_view url,
    IceoryxBus::MsgHandleFunc callback,
    IoxSubscriberRegistrationOptions options,
    std::shared_ptr<IoxBusControl> control)
    : control_(std::move(control)),
      url_(url),
      subscriber_(std::make_unique<iox::popo::UntypedSubscriber>(
          Url2ServiceDescription(url), MakeSubscriberOptions(options.retained))),
      callback_(std::move(callback)),
      lease_limiter_(options.max_outstanding),
      retained_(options.retained),
      async_stop_([this] {
        if (callback_context_) callback_context_->Close();
        DetachFromListener();
        if (callback_context_) callback_context_->WaitForQuiescence();
        WaitForQuiescence();
        Deregister();
      }) {}

IoxSubscriptionState::~IoxSubscriptionState() = default;

bool IoxSubscriptionState::Quiescent() const noexcept {
  return active_callbacks_ == 0 && lease_limiter_.Outstanding() == 0;
}

bool IoxSubscriptionState::WaitForQuiescence() noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return Quiescent(); });
  return true;
}

void IoxSubscriptionState::Stop() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
  }
  // The worker owns a strong reference until detach, callback quiescence and
  // registry removal are complete. Therefore the raw notification context
  // cannot be destroyed while the listener can still enter it.
  auto self = weak_from_this().lock();
  if (self) async_stop_.Request(std::move(self));
}

void IoxSubscriptionState::DetachFromListener() noexcept {
  std::shared_ptr<iox::popo::Listener> listener;
  if (control_) {
    std::lock_guard<std::mutex> control_lock(control_->mutex);
    listener = control_->listener;
  }
  if (listener) {
    listener->detachEvent(Subscriber(),
                          iox::popo::SubscriberEvent::DATA_RECEIVED);
  }
}

void IoxSubscriptionState::Deregister() noexcept {
  auto control = control_;
  if (!control) return;
  std::lock_guard<std::mutex> lock(control->mutex);
  control->subscriptions.Remove(url_, this);
}

bool IoxSubscriptionState::Close(std::chrono::milliseconds timeout) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Stop();
  std::unique_lock<std::mutex> lock(mutex_);
  if (current_callback_subscription == this) return false;
  lock.unlock();
  if (timeout.count() <= 0) {
    lock.lock();
    return async_stop_.Complete() && Quiescent();
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0 || !async_stop_.Close(remaining)) return false;
  lock.lock();
  remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) return Quiescent();
  return cv_.wait_for(lock, remaining, [this] { return Quiescent(); });
}

void IoxSubscriptionState::Release(const void* payload) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (payload != nullptr) subscriber_->release(payload);
  lease_limiter_.Release();
  cv_.notify_all();
}

void IoxSubscriptionState::SetCallbackContext(
    std::shared_ptr<IoxSubscriptionCallbackContext> context) noexcept {
  callback_context_ = std::move(context);
}

void IoxSubscriptionState::OnData() noexcept {
  while (true) {
    const void* payload = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || !subscriber_->hasData()) break;

      auto take_result = subscriber_->take();
      if (take_result.has_error()) break;
      payload = take_result.value();

      if (!lease_limiter_.TryAcquire()) {
        subscriber_->release(payload);
        continue;
      }
      ++active_callbacks_;
    }

    internal::RunIoxAcquiredPayload(
        [this, payload]() noexcept { Release(payload); },
        [this]() noexcept {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_callbacks_;
          }
          cv_.notify_all();
        },
        [this, payload](bool& release_on_exit) {
          if (!retained_) {
            struct ScopedRelease {
              IoxSubscriptionState* state;
              const void* payload;
              bool released = false;
            } scoped_release{this, payload};
            auto release = std::function<void()>([scope = &scoped_release] {
              if (!scope->released) {
                scope->released = true;
                scope->state->Release(scope->payload);
              }
            });
            auto chunk_header = iox::mepoo::ChunkHeader::fromUserPayload(payload);
            const size_t payload_size =
                chunk_header ? chunk_header->userPayloadSize() : 0;
            auto* previous_callback_subscription = current_callback_subscription;
            current_callback_subscription = this;
            try {
              callback_(payload, payload_size, std::move(release));
            } catch (...) {
              current_callback_subscription = previous_callback_subscription;
              if (!scoped_release.released) Release(payload);
              release_on_exit = false;
              throw;
            }
            current_callback_subscription = previous_callback_subscription;
            if (!scoped_release.released) Release(payload);
            release_on_exit = false;
            return;
          }
          std::shared_ptr<IoxReleaseToken> token;
          try {
            auto self = shared_from_this();
            token = std::make_shared<IoxReleaseToken>(
                [self, payload] { self->Release(payload); });
            release_on_exit = false;

            auto chunk_header =
                iox::mepoo::ChunkHeader::fromUserPayload(payload);
            const size_t payload_size =
                chunk_header ? chunk_header->userPayloadSize() : 0;
            auto release =
                std::function<void()>([token] { token->Release(); });

            auto* previous_callback_subscription =
                current_callback_subscription;
            current_callback_subscription = this;
            try {
              callback_(payload, payload_size, std::move(release));
            } catch (...) {
              current_callback_subscription = previous_callback_subscription;
              throw;
            }
            current_callback_subscription = previous_callback_subscription;
          } catch (const std::exception& e) {
            MW_WARN("Iceoryx subscriber callback setup or callback threw: {}",
                    e.what());
            if (token) token->Release();
          } catch (...) {
            MW_WARN("Iceoryx subscriber callback setup or callback threw an unknown exception.");
            if (token) token->Release();
          }
        });
  }
}

void IoxSubscriptionCallbackContext::Invoke() noexcept {
  if (!gate_.TryEnter()) return;
  struct ExitGuard {
    IoxNotificationGate& gate;
    ~ExitGuard() { gate.Exit(); }
  } exit{gate_};
  if (auto state = state_.lock()) state->OnData();
}

}  // namespace ibmw::extensions::iceoryx_extension
