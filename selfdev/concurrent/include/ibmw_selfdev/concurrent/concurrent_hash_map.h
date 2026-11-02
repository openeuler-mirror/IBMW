// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "ibmw_selfdev/concurrent/detail/adaptive_lock.h"

namespace ibmw::selfdev::concurrent {

/// Hash map sharded over independently locked std::unordered_map instances.
/// Replaces tbb::concurrent_hash_map. Instead of accessors, values are reached
/// through Visit()/Take() so a lock never outlives the call.
template <class Key, class Value, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
class ConcurrentHashMap {
 public:
  static constexpr size_t kMaxShards = size_t{1} << 16;
  /// Enough that 16 busy threads rarely meet on a shard (16 shards cost up to
  /// 2x against tbb at 16 threads, see 性能对比.md); ~8 KiB of shard headers.
  static constexpr size_t kDefaultShards = 64;

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
  template <class V>
  bool InsertOrAssign(const Key& key, V&& value) {
    Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    return s.map.insert_or_assign(key, std::forward<V>(value)).second;
  }

  /// Atomically finds, removes and returns the value.
  std::optional<Value> Take(const Key& key) {
    Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    auto it = s.map.find(key);
    if (it == s.map.end()) return std::nullopt;
    std::optional<Value> out(std::move(it->second));
    s.map.erase(it);
    return out;
  }

  bool Erase(const Key& key) {
    Shard& s = ShardFor(key);
    std::lock_guard<detail::AdaptiveMutex> lock(s.mu);
    return s.map.erase(key) != 0;
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
      std::unordered_map<Key, Value, Hash, KeyEqual> dropped(0, hash_);
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
  struct alignas(64) Shard {
    explicit Shard(const Hash& hash) : map(0, hash) {}
    mutable detail::AdaptiveMutex mu;
    std::unordered_map<Key, Value, Hash, KeyEqual> map;
  };

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
