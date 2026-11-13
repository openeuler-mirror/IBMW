// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace ibmw::util {

/**
 * @brief Append-only vector whose elements never move.
 * @note Storage is a fixed table of geometrically growing chunks, so
 * emplace_back() never relocates existing elements: references and indices
 * handed out earlier stay valid while other threads keep appending.
 * Appends are serialized internally; element access is lock-free.
 */
template <class T>
class StableVector {
 public:
  StableVector() = default;
  ~StableVector() {
    const size_t n = size_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) (*this)[i].~T();
    for (auto& c : chunks_) {
      if (T* p = c.load(std::memory_order_relaxed)) ::operator delete(p, std::align_val_t{alignof(T)});
    }
  }

  StableVector(const StableVector&) = delete;
  StableVector& operator=(const StableVector&) = delete;

  /// Appends an element and returns its index (stable for the container's lifetime).
  template <class... Args>
  size_t emplace_back(Args&&... args) {
    std::lock_guard<std::mutex> lck(mutex_);
    const size_t idx = size_.load(std::memory_order_relaxed);
    const auto [k, off] = Locate(idx);
    if (k >= kMaxChunks) [[unlikely]]
      throw std::length_error("StableVector capacity exceeded.");
    T* chunk = chunks_[k].load(std::memory_order_relaxed);
    if (chunk == nullptr) {
      chunk = static_cast<T*>(::operator new(sizeof(T) * ChunkSize(k), std::align_val_t{alignof(T)}));
      chunks_[k].store(chunk, std::memory_order_release);
    }
    new (chunk + off) T(std::forward<Args>(args)...);
    size_.store(idx + 1, std::memory_order_release);
    return idx;
  }

  T& operator[](size_t idx) { return *Ptr(idx); }
  const T& operator[](size_t idx) const { return *Ptr(idx); }

  size_t size() const { return size_.load(std::memory_order_acquire); }

 private:
  static constexpr size_t kFirstChunkBits = 5;  // first chunk holds 32 elements
  // ~2^32 elements on 64-bit; on 32-bit keep the largest chunk (2^20 elements)
  // small enough that its byte size cannot overflow size_t. ~2M elements.
  static constexpr size_t kMaxChunks = sizeof(size_t) >= 8 ? 27 : 16;

  static constexpr size_t ChunkSize(size_t k) { return size_t{1} << (k + kFirstChunkBits); }

  // Chunk k covers indices [32 * (2^k - 1), 32 * (2^(k+1) - 1)).
  static constexpr std::pair<size_t, size_t> Locate(size_t idx) {
    const size_t biased = idx + ChunkSize(0);
    const size_t k = static_cast<size_t>(std::bit_width(biased)) - 1 - kFirstChunkBits;
    return {k, biased - ChunkSize(k)};
  }

  T* Ptr(size_t idx) const {
    if (idx < ChunkSize(0)) [[likely]]  // typical: a handful of resources
      return chunks_[0].load(std::memory_order_acquire) + idx;
    const auto [k, off] = Locate(idx);
    return chunks_[k].load(std::memory_order_acquire) + off;
  }

  std::array<std::atomic<T*>, kMaxChunks> chunks_{};
  std::atomic<size_t> size_{0};
  std::mutex mutex_;
};

}  // namespace ibmw::util
