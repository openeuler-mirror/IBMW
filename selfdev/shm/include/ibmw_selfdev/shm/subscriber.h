// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ibmw_selfdev/shm/constants.h"
#include "ibmw_selfdev/shm/result.h"

namespace ibmw::selfdev::shm {

namespace detail {
class SubscriberImpl;
class ListenerImpl;
}

enum class QueueFullPolicy : uint8_t { kDiscardOldest, kBlockProducer };

struct SubscriberOptions {
  uint32_t queue_capacity = 1;  // clamped to [1, kMaxSubscriberQueueCapacity]
  QueueFullPolicy queue_full = QueueFullPolicy::kDiscardOldest;
  uint32_t max_held = kMaxChunksHeldPerSubscriber;  // clamped to [1, 256]
};

struct SubscriberStats {
  uint64_t takes = 0;
  uint64_t releases = 0;
  uint64_t return_ring_full = 0;       // returns parked because the ring was full
  uint64_t corrupted_connections = 0;  // publishers dropped for protocol violations
  uint32_t connections = 0;  // publishers currently connected and ready
  uint32_t held = 0;
};

// Zero-copy subscriber of one URL; connects to every publisher of that URL
// (N:M). Not copyable. Thread-safe.
class Subscriber {
 public:
  explicit Subscriber(std::string_view url, const SubscriberOptions& options = {});
  ~Subscriber();
  Subscriber(const Subscriber&) = delete;
  Subscriber& operator=(const Subscriber&) = delete;

  bool Valid() const noexcept;
  Error InitError() const noexcept;

  bool HasData() const noexcept;
  // Returns a read-only payload; release it with Release(). Payloads must be
  // released before the Subscriber is destroyed: destruction unmaps them.
  Result<const void*> Take() noexcept;
  void Release(const void* user_payload) noexcept;
  // Size of a held payload as validated by Take() against the chunk stride;
  // 0 if the payload is not held. Use this instead of ChunkView on the
  // subscriber side: chunk headers stay writable by the publisher.
  size_t PayloadSize(const void* user_payload) const noexcept;

  SubscriberStats Stats() const noexcept;

 private:
  friend class detail::ListenerImpl;
  friend class Listener;
  std::shared_ptr<detail::SubscriberImpl> impl_;
  Error init_error_ = Error::kOk;
};

}  // namespace ibmw::selfdev::shm
