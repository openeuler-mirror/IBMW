// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "ibmw_selfdev/shm/constants.h"
#include "ibmw_selfdev/shm/result.h"

namespace ibmw::selfdev::shm {

namespace detail {
class PublisherImpl;
}

enum class TooSlowPolicy : uint8_t { kDiscardOldest, kWaitForConsumer };

struct PoolConfig {
  uint32_t payload_size = 0;   // rounded up to the next power-of-two class
  uint32_t initial_count = 0;  // chunks created eagerly at construction
};

struct PublisherOptions {
  TooSlowPolicy too_slow = TooSlowPolicy::kDiscardOldest;
  std::vector<PoolConfig> pools;               // optional eager pools
  uint64_t max_bytes = 512ull << 20;           // shared memory budget
  bool prefault = false;                       // MAP_POPULATE new segments
  // Upper bound for one Publish() blocked on a full lossless subscriber;
  // zero waits until the subscriber takes, disconnects or this publisher
  // is destroyed.
  std::chrono::milliseconds block_timeout{0};
};

struct PublisherStats {
  uint64_t loans = 0;
  uint64_t publishes = 0;
  uint64_t deliveries = 0;      // successful pushes into subscriber queues
  uint64_t evictions = 0;       // DISCARD_OLDEST overflow
  uint64_t blocked = 0;         // Publish() calls that had to wait
  uint64_t block_timeouts = 0;
  uint64_t pool_exhausted = 0;
  uint64_t bad_returns = 0;     // invalid/duplicate handles from subscribers
  uint64_t reclaimed = 0;       // chunks reclaimed from dead subscribers
  uint64_t corrupted_connections = 0;
  uint32_t connections = 0;
  uint32_t segments = 0;
  uint64_t segment_bytes = 0;
  bool read_only_enforced = true;  // F_SEAL_FUTURE_WRITE applied to all segments
};

// Zero-copy publisher of one URL. Not copyable. Thread-safe.
class Publisher {
 public:
  explicit Publisher(std::string_view url, const PublisherOptions& options = {});
  ~Publisher();
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  // False when construction failed (see InitError()).
  bool Valid() const noexcept;
  Error InitError() const noexcept;

  Result<void*> Loan(uint32_t payload_size, uint32_t alignment = kDefaultPayloadAlignment) noexcept;
  // Publishes a loaned chunk and gives up the loan. Never copies.
  void Publish(void* user_payload) noexcept;
  // Returns a loaned chunk without publishing it.
  void Release(void* user_payload) noexcept;

  bool HasSubscribers() const noexcept;
  PublisherStats Stats() const noexcept;

 private:
  std::shared_ptr<detail::PublisherImpl> impl_;
  Error init_error_ = Error::kOk;
};

}  // namespace ibmw::selfdev::shm
