// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ibmw_selfdev/concurrent/detail/adaptive_lock.h"
#include "ibmw_selfdev/concurrent/detail/node_cache.h"

namespace ibmw::selfdev::concurrent {

/// Hash map sharded over independently locked std::unordered_map instances.
/// Replaces tbb::concurrent_hash_map. Instead of accessors, values are reached
/// through Visit()/Take() so a lock never outlives the call.
template <class Key, class Value, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
class ConcurrentHashMap {
 public:
  static constexpr size_t kMaxShards = size_t{1} << 16;
  /// Sized for large, busy maps: with random keys each access moves the
  /// shard's lock cache line between cores, and fewer shards make that the
  /// bottleneck (on a 20-core aarch64 OHOS board, Emplace + Take of distinct
  /// keys over 100k entries at 16 threads, with the node cache: 64 shards
  /// 0.29-0.36x tbb's time, 512 shards 0.08-0.11x). Costs ~64 KiB of shard
  /// headers with libc++ (~96 KiB with libstdc++); maps with few live entries
  /// can pass a smaller count.
  static constexpr size_t kDefaultShards = 512;

  /// Rounded up to a power of two and clamped to [1, kMaxShards]. `hash` is
  /// used for shard selection and copied into every shard.
  explicit ConcurrentHashMap(size_t shard_count = kDefaultShards, const Hash& hash = Hash())
      : hash_(hash), shard_count_(RoundUpPow2(shard_count)) {
    // deque: shards hold a mutex (not movable) and Hash need not be default
    // constructible.
    for (size_t i = 0; i < shard_count_; ++i) shards_.emplace_back(hash_);
  }

  ConcurrentHashMap(const ConcurrentHashMap&) = delete;
  ConcurrentHashMap& operator=(const ConcurrentHashMap&) = delete;

  /// Inserts when the key is absent and returns true. When present returns
  /// false without constructing from (or moving) args.
  template <class... Args>
  bool Emplace(const Key& key, Args&&... args) {
    Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    return s.map.try_emplace(key, std::forward<Args>(args)...).second;
  }

  /// Returns true when inserted, false when an existing value was replaced.
  /// The replaced value is destroyed after the shard lock is released. The
  /// new value is constructed from `value` before the lock is taken, then
  /// moved in or swapped with the old one: Value must be constructible from
  /// V, move constructible and swappable. If constructing it throws, the
  /// map is unchanged.
  template <class V>
  bool InsertOrAssign(const Key& key, V&& value) {
    static_assert(std::is_constructible_v<Value, V&&>, "InsertOrAssign: Value must be constructible from the argument");
    static_assert(std::is_move_constructible_v<Value> && std::is_swappable_v<Value>,
                  "InsertOrAssign: Value must be move constructible and swappable (the old value is swapped out "
                  "under the shard lock and destroyed after it)");
    Value fresh(std::forward<V>(value));
    Shard& s = ShardFor(key);
    std::unique_lock<detail::AdaptiveMutex> lock(s.mu);
    // try_emplace leaves `fresh` alone when the key is present.
    auto [it, inserted] = s.map.try_emplace(key, std::move(fresh));
    if (!inserted) {
      using std::swap;
      swap(it->second, fresh);
    }
    lock.unlock();
    return inserted;
  }

  /// Atomically finds, removes and returns the value. The moved-from value
  /// is destroyed after the shard lock is released; if moving it out
  /// throws, the key stays in the map.
  std::optional<Value> Take(const Key& key) {
    Shard& s = ShardFor(key);
    std::unique_lock<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    std::optional<Value> out(std::move(it->second));
    auto node = s.map.extract(it);
    lock.unlock();
    return out;
  }

  /// Take() only if pred(const Value&) holds, checked under the same shard
  /// lock; otherwise the value stays. pred must not access this map.
  template <class Pred>
  std::optional<Value> TakeIf(const Key& key, Pred&& pred) {
    Shard& s = ShardFor(key);
    std::unique_lock<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end() || !pred(std::as_const(it->second))) return std::nullopt;
    std::optional<Value> out(std::move(it->second));
    auto node = s.map.extract(it);
    lock.unlock();
    return out;
  }

  /// The value is destroyed after the shard lock is released.
  bool Erase(const Key& key) {
    Shard& s = ShardFor(key);
    std::unique_lock<detail::AdaptiveMutex> lock(s.mu);
    auto node = s.map.extract(key);
    lock.unlock();
    return !node.empty();
  }

  bool Contains(const Key& key) const {
    const Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    return s.map.find(key) != s.map.end();
  }

  /// Returns a copy of the value.
  std::optional<Value> Get(const Key& key) const {
    const Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    return it->second;
  }

  /// Calls fn(Value&) under the shard lock; returns false if the key is
  /// absent. fn must not access this map (deadlock).
  template <class Fn>
  bool Visit(const Key& key, Fn&& fn) {
    Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    std::forward<Fn>(fn)(it->second);
    return true;
  }

  template <class Fn>
  bool Visit(const Key& key, Fn&& fn) const {
    const Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    std::forward<Fn>(fn)(std::as_const(it->second));
    return true;
  }

  /// Calls fn(const Key&, Value&) for every entry, one shard at a time (not a
  /// global snapshot). fn must not access this map.
  template <class Fn>
  void ForEach(Fn&& fn) {
    for (size_t i = 0; i < shard_count_; ++i) {
      std::lock_guard<detail::AdaptiveMutex> lock(shards_[i].mu);
      for (auto& [k, v] : shards_[i].map) fn(k, v);
    }
  }

  /// Sum over shards; approximate while other threads modify the map.
  size_t Size() const {
    size_t n = 0;
    for (size_t i = 0; i < shard_count_; ++i) {
      std::lock_guard<detail::AdaptiveMutex> lock(shards_[i].mu);
      n += shards_[i].map.size();
    }
    return n;
  }

  bool Empty() const { return Size() == 0; }

  void Clear() {
    for (size_t i = 0; i < shard_count_; ++i) {
      ShardMap dropped(0, hash_, KeyEqual(), typename ShardMap::allocator_type(&shards_[i].nodes));
      {
        std::lock_guard<detail::AdaptiveMutex> lock(shards_[i].mu);
        dropped.swap(shards_[i].map);
      }
    }
  }

  size_t ShardCount() const { return shard_count_; }

  /// Shard holding `key`; exposed for diagnostics and distribution tests.
  size_t ShardIndex(const Key& key) const {
    return static_cast<size_t>(Mix(static_cast<uint64_t>(hash_(key)))) & (shard_count_ - 1);
  }

 private:
  using ShardMap =
      std::unordered_map<Key, Value, Hash, KeyEqual, detail::NodeCacheAllocator<std::pair<const Key, Value>>>;

  struct alignas(64) Shard {
    explicit Shard(const Hash& hash) : map(0, hash, KeyEqual(), typename ShardMap::allocator_type(&nodes)) {}
    mutable detail::AdaptiveMutex mu;
    detail::NodeCache nodes;  // before map: the map returns its nodes here when destroyed
    ShardMap map;
  };
#if defined(__OHOS__) && defined(_LIBCPP_VERSION) && defined(__aarch64__)
  // The size kDefaultShards and NodeCache::kCapacity were measured with.
  static_assert(!std::is_empty_v<Hash> || !std::is_empty_v<KeyEqual> || sizeof(Shard) == 128,
                "shard outgrew two cache lines");
#endif

  static size_t RoundUpPow2(size_t n) {
    size_t p = 1;
    while (p < n && p < kMaxShards) p <<= 1;
    return p;
  }

  // murmur3 fmix64: spreads identity hashes such as std::hash<uint32_t>.
  static uint64_t Mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }

  Shard& ShardFor(const Key& key) { return shards_[ShardIndex(key)]; }
  const Shard& ShardFor(const Key& key) const { return shards_[ShardIndex(key)]; }

  Hash hash_;
  size_t shard_count_;
  std::deque<Shard> shards_;
};

}  // namespace ibmw::selfdev::concurrent
