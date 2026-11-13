// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace ibmw::selfdev::concurrent::detail {

/// Keeps a few freed blocks of one size for reuse, so a map that keeps
/// inserting and removing entries does not call malloc and free for every
/// node. On the OHOS board musl's allocator dominates Emplace + Take: a global
/// lock under contention, and pages handed back with madvise and faulted in
/// again when a table stays small.
///
/// Never blocks: if another thread is using the cache, the caller falls back
/// to operator new / delete. Thread safe on its own, so nodes can be released
/// outside the shard lock.
///
/// Cached blocks are not poisoned: ASAN does not catch a use after free of
/// a node that is still cached.
class NodeCache {
 public:
  /// Two blocks keep a shard at 128 bytes (with std::mutex and libc++ on
  /// aarch64) and cover an Emplace + Take churn of up to two entries per shard.
  static constexpr uint8_t kCapacity = 2;

  NodeCache() = default;
  NodeCache(const NodeCache&) = delete;
  NodeCache& operator=(const NodeCache&) = delete;

  ~NodeCache() {
    for (uint8_t i = 0; i < count_; ++i) ::operator delete(blocks_[i]);
  }

  /// A cached block of `size` bytes, or nullptr.
  void* Pop(size_t size) noexcept {
    if (!TryLock()) return nullptr;
    void* p = nullptr;
    if (count_ != 0 && size == block_size_) p = blocks_[--count_];
    Unlock();
    return p;
  }

  /// Takes `p` (from ::operator new(size)) if there is room; false otherwise.
  bool Push(void* p, size_t size) noexcept {
    if (size > UINT32_MAX || !TryLock()) return false;
    if (count_ == 0) block_size_ = static_cast<uint32_t>(size);
    const bool kept = size == block_size_ && count_ < kCapacity;
    if (kept) blocks_[count_++] = p;
    Unlock();
    return kept;
  }

 private:
  bool TryLock() noexcept {
    return !busy_.load(std::memory_order_relaxed) && !busy_.exchange(true, std::memory_order_acquire);
  }
  void Unlock() noexcept { busy_.store(false, std::memory_order_release); }

  std::atomic<bool> busy_{false};
  uint8_t count_ = 0;
  uint32_t block_size_ = 0;  // of the cached blocks; set by a Push into an empty cache
  void* blocks_[kCapacity];
};

/// Allocator that routes single-object allocations through a NodeCache. All
/// copies (and rebinds) share the cache, which must outlive the container.
template <class T>
class NodeCacheAllocator {
 public:
  using value_type = T;

  explicit NodeCacheAllocator(NodeCache* cache) noexcept : cache_(cache) {}
  template <class U>
  NodeCacheAllocator(const NodeCacheAllocator<U>& other) noexcept : cache_(other.cache_) {}

  T* allocate(size_t n) {
    if (Cacheable(n)) {
      if (void* p = cache_->Pop(sizeof(T))) return static_cast<T*>(p);
    }
    return std::allocator<T>().allocate(n);
  }

  void deallocate(T* p, size_t n) noexcept {
    if (Cacheable(n) && cache_->Push(p, sizeof(T))) return;
    std::allocator<T>().deallocate(p, n);
  }

  template <class U>
  bool operator==(const NodeCacheAllocator<U>& other) const noexcept {
    return cache_ == other.cache_;
  }
  template <class U>
  bool operator!=(const NodeCacheAllocator<U>& other) const noexcept {
    return cache_ != other.cache_;
  }

 private:
  template <class U>
  friend class NodeCacheAllocator;

  // std::allocator<T> gets these blocks from plain ::operator new(sizeof(T)),
  // which is what NodeCache frees them with.
  static constexpr bool Cacheable(size_t n) noexcept {
    return n == 1 && alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__;
  }

  NodeCache* cache_;
};

}  // namespace ibmw::selfdev::concurrent::detail
