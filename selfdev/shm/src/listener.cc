// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <pthread.h>

#include <algorithm>
#include <future>

#include "impl.h"

namespace ibmw::selfdev::shm {

Listener::Listener(const ListenerOptions& options) {
  auto r = detail::ListenerImpl::Create(options);
  if (r) {
    impl_ = std::move(r.value());
  } else {
    init_error_ = r.error();
  }
}

Listener::~Listener() {
  if (impl_) impl_->Stop();
}

bool Listener::Valid() const noexcept { return impl_ != nullptr; }
Error Listener::InitError() const noexcept { return init_error_; }

Result<void> Listener::Attach(Subscriber& subscriber, Callback callback, void* context) noexcept {
  if (!impl_) return Error::kNotConnected;
  return impl_->Attach(subscriber, callback, context);
}

void Listener::Detach(Subscriber& subscriber) noexcept {
  if (impl_ && subscriber.impl_) impl_->Detach(subscriber.impl_.get());
}

uint32_t Listener::Size() const noexcept { return impl_ ? impl_->Size() : 0; }

namespace detail {

Result<std::shared_ptr<ListenerImpl>> ListenerImpl::Create(const ListenerOptions& options) {
  auto* rt = RuntimeImpl::Instance();
  if (rt == nullptr) return Error::kSystem;
  auto slot = rt->AllocListenerSlot();
  if (!slot) return Error::kNoMemory;
  std::shared_ptr<ListenerImpl> self(new ListenerImpl());
  self->rt_ = rt;
  self->options_ = options;
  self->slot_ = *slot;
  self->notify_ = &rt->Notify()->slot[*slot];
  self->notify_->sleeping.store(0, std::memory_order_relaxed);
  std::promise<std::thread::id> started;
  auto f = started.get_future();
  // The thread owns a reference: a callback may destroy the Listener, after
  // which the loop still has to unwind on this object.
  self->thread_ = std::thread([keep = self, &started] {
    std::string name = keep->options_.thread_name.substr(0, 15);
    ::pthread_setname_np(::pthread_self(), name.c_str());
    started.set_value(std::this_thread::get_id());
    keep->Loop();
    keep->Cleanup();
  });
  self->thread_id_ = f.get();
  return self;
}

ListenerImpl::~ListenerImpl() { Stop(); }

void ListenerImpl::Stop() noexcept {
  if (!thread_.joinable()) return;
  stop_.store(true, std::memory_order_seq_cst);
  Wake();
  if (std::this_thread::get_id() == thread_id_) {
    // Destroyed from its own callback: the loop exits after the callback
    // returns and cleans up there.
    thread_.detach();
  } else {
    thread_.join();
  }
}

// Runs on the listener thread once the loop has exited, so the notify slot
// is only reused after this thread stopped touching it.
void ListenerImpl::Cleanup() noexcept {
  notify_->sleeping.store(0, std::memory_order_relaxed);
  std::vector<Entry> entries;
  {
    std::lock_guard<std::mutex> lock(mu_);
    entries.swap(entries_);
  }
  for (auto& e : entries) {
    e.impl->SetNotifySlot(kNoListener);
    e.impl->ClearListener(this);
  }
  rt_->FreeListenerSlot(slot_);
}

void ListenerImpl::Wake() noexcept {
  notify_->epoch.fetch_add(1, std::memory_order_seq_cst);
  FutexWake(&notify_->epoch, 1);
}

Result<void> ListenerImpl::Attach(Subscriber& sub, Listener::Callback cb, void* ctx) noexcept {
  if (cb == nullptr || !sub.impl_) return Error::kInvalidArg;
  auto impl = sub.impl_;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_.load()) return Error::kShuttingDown;
    for (auto& e : entries_) {
      if (e.impl == impl) return Error::kInvalidArg;
    }
    if (entries_.size() >= options_.capacity) return Error::kNoMemory;
    if (!impl->ClaimListener(shared_from_this())) return Error::kInvalidArg;  // attached elsewhere
    // ~0: fire once if data is already waiting at attach time.
    entries_.push_back(Entry{&sub, impl, cb, ctx, ~0ull});
  }
  impl->SetNotifySlot(slot_);
  Wake();
  return {};
}

void ListenerImpl::Detach(SubscriberImpl* sub) noexcept {
  std::shared_ptr<SubscriberImpl> impl;
  std::unique_lock<std::mutex> lock(mu_);
  auto it = std::find_if(entries_.begin(), entries_.end(), [sub](const Entry& e) { return e.impl.get() == sub; });
  if (it == entries_.end()) return;
  impl = std::move(it->impl);
  entries_.erase(it);
  impl->ClearListener(this);
  if (std::this_thread::get_id() != thread_id_) {
    cv_.wait(lock, [&] { return dispatching_ != sub; });
  }
  lock.unlock();
  impl->SetNotifySlot(kNoListener);
}

uint32_t ListenerImpl::Size() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<uint32_t>(entries_.size());
}

void ListenerImpl::Loop() noexcept {
  struct Ready {
    const SubscriberImpl* impl;
    uint64_t version;
  };
  std::vector<Ready> ready;
  while (true) {
    // Read the epoch BEFORE announcing sleep: if a publisher bumps it after
    // our HasData check, futex_wait sees the new value and returns at once.
    uint32_t e = notify_->epoch.load(std::memory_order_acquire);
    notify_->sleeping.store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);  // store->load, pairs with WakeListener
    if (stop_.load(std::memory_order_relaxed)) break;
    ready.clear();
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto& en : entries_) {
        uint64_t v;
        if (en.impl->Poll(&v) && v != en.last_version) ready.push_back({en.impl.get(), v});
      }
    }
    if (ready.empty()) {
      FutexWait(&notify_->epoch, e, -1);
      notify_->sleeping.store(0, std::memory_order_relaxed);
      continue;
    }
    notify_->sleeping.store(0, std::memory_order_relaxed);
    for (auto& r : ready) {
      Subscriber* sub = nullptr;
      Listener::Callback cb = nullptr;
      void* ctx = nullptr;
      {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& en) { return en.impl.get() == r.impl; });
        if (it == entries_.end()) continue;
        it->last_version = r.version;
        sub = it->subscriber;
        cb = it->callback;
        ctx = it->context;
        dispatching_ = r.impl;
      }
      cb(sub, ctx);
      {
        std::lock_guard<std::mutex> lock(mu_);
        dispatching_ = nullptr;
      }
      cv_.notify_all();
      if (stop_.load(std::memory_order_relaxed)) break;
    }
  }
}

}  // namespace detail
}  // namespace ibmw::selfdev::shm
