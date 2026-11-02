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
  // Fail (Loan()/construction return kSystem) instead of creating a data
  // segment that subscribers could map writable when the kernel lacks
  // F_SEAL_FUTURE_WRITE (Linux < 5.1). Default: degrade, warn once and count
  // in PublisherStats::unsealed_segments.
  bool require_read_only = false;
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
  uint64_t reclaimed = 0;       // chunks reclaimed from closed or dead subscribers
  uint64_t corrupted_connections = 0;
  uint32_t connections = 0;
  uint32_t segments = 0;
  uint64_t segment_bytes = 0;
  bool read_only_enforced = true;  // F_SEAL_FUTURE_WRITE applied to all segments
  uint64_t unsealed_segments = 0;  // data segments created without F_SEAL_FUTURE_WRITE
  // A subscriber that disconnects without an orderly close may still be
  // reading what it holds: those chunks are quarantined, not reused, until
  // its process is known to be gone (then they count as reclaimed).
  uint64_t quarantined = 0;          // chunks ever quarantined
  uint32_t quarantined_chunks = 0;   // currently quarantined
  // Quarantines whose subscriber process cannot be checked (other pid
  // namespace, same process, sandbox): held until it closes in order or
  // this publisher is destroyed.
  uint64_t unverifiable_peers = 0;
  uint64_t unknown_messages = 0;     // control messages of unknown type, ignored
  uint64_t deferred_sends = 0;       // control messages queued: subscriber socket full
  uint64_t send_queue_overflows = 0; // connections dropped: that queue hit its bound
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
  // Publishes a loaned chunk and gives up the loan. Never copies. False (and
  // nothing happens) if user_payload is not a chunk currently on loan.
  bool Publish(void* user_payload) noexcept;
  // Returns a loaned chunk without publishing it. False as for Publish().
  bool Release(void* user_payload) noexcept;
  // Ends lossless blocking for good: blocked Publish() calls return now and
  // later ones skip full lossless subscribers (counted as block_timeouts).
  // For shutdown when a remote consumer may never drain its queue.
  void StopBlocking() noexcept;

  bool HasSubscribers() const noexcept;
  PublisherStats Stats() const noexcept;

 private:
  std::shared_ptr<detail::PublisherImpl> impl_;
  Error init_error_ = Error::kOk;
};

}  // namespace ibmw::selfdev::shm
