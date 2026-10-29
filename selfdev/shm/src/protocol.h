// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ibmw_selfdev/shm/constants.h"

// Shared-memory layouts and control messages. Every value read from memory a
// peer can write is range-checked by the reader before use.
namespace ibmw::selfdev::shm::detail {

inline constexpr uint32_t kConnMagic = 0x4e4e4f43;    // "CONN"
inline constexpr uint32_t kNotifyMagic = 0x5946544e;  // "NTFY"
inline constexpr uint32_t kMsgMagic = 0x4d534849;     // "IHSM"
inline constexpr uint32_t kNoListener = 0xffffffffu;

// ---- chunk handle: segment 12 bit | chunk index 20 bit | generation 32 bit
inline constexpr uint64_t MakeHandle(uint32_t seg, uint32_t idx, uint32_t gen) noexcept {
  return (static_cast<uint64_t>(seg & 0xfffu) << 52) | (static_cast<uint64_t>(idx & 0xfffffu) << 32) |
         gen;
}
inline constexpr uint32_t HandleSegment(uint64_t h) noexcept { return static_cast<uint32_t>(h >> 52); }
inline constexpr uint32_t HandleIndex(uint64_t h) noexcept {
  return static_cast<uint32_t>((h >> 32) & 0xfffffu);
}
inline constexpr uint32_t HandleGeneration(uint64_t h) noexcept { return static_cast<uint32_t>(h); }

// ---- per-process listener notification segment (owned by the subscriber
// process, mapped read-write by every publisher connected to it).
struct alignas(64) NotifySlot {
  std::atomic<uint32_t> epoch;     // futex word
  std::atomic<uint32_t> sleeping;  // 1 while the listener may block
};
struct NotifySegment {
  uint32_t magic;
  uint32_t slots;
  uint8_t pad[56];
  NotifySlot slot[kMaxListenersPerProcess];
};
inline constexpr size_t kNotifySegmentSize = sizeof(NotifySegment);

// ---- per-connection segment (created by the publisher, shared read-write
// with exactly one subscriber).
//  delivery ring: publisher -> subscriber, `delivery_slots` = capacity + 1
//  return ring:   subscriber -> publisher, `return_slots` = capacity + held + 1
struct ConnHeader {
  uint32_t magic;
  uint32_t abi;
  uint32_t queue_capacity;
  uint32_t delivery_slots;
  uint32_t return_slots;
  uint32_t pad0[11];
  alignas(64) std::atomic<uint64_t> delivery_write;  // publisher only
  alignas(64) std::atomic<uint64_t> delivery_read;   // CAS by both sides
  alignas(64) std::atomic<uint64_t> return_write;    // subscriber only
  alignas(64) std::atomic<uint64_t> return_read;     // publisher only
  alignas(64) std::atomic<uint32_t> notify_slot;     // subscriber: listener slot or kNoListener
  alignas(64) std::atomic<uint32_t> space_epoch;     // futex word for lossless producer
  std::atomic<uint32_t> space_sleeping;
  alignas(64) std::atomic<uint32_t> closed;          // set by either side on shutdown
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

inline size_t ConnSegmentSize(uint32_t delivery_slots, uint32_t return_slots) noexcept {
  return sizeof(ConnHeader) + sizeof(uint64_t) * (static_cast<size_t>(delivery_slots) + return_slots);
}
inline std::atomic<uint64_t>* DeliverySlots(ConnHeader* h) noexcept {
  return reinterpret_cast<std::atomic<uint64_t>*>(reinterpret_cast<uint8_t*>(h) + sizeof(ConnHeader));
}
inline std::atomic<uint64_t>* ReturnSlots(ConnHeader* h, uint32_t delivery_slots) noexcept {
  return DeliverySlots(h) + delivery_slots;
}

// ---- control messages over the SOCK_SEQPACKET connection
enum class MsgType : uint32_t {
  kHello = 1,    // S->P, fd: notify segment
  kWelcome,      // P->S, fd: connection segment
  kConnAck,      // S->P
  kSegmentAdd,   // P->S, fd: data segment
  kSegmentAck,   // S->P
  kReject,       // P->S, then close
};

struct MsgHeader {
  uint32_t magic;
  MsgType type;
};

struct HelloMsg {
  MsgHeader h;
  uint32_t abi;
  uint32_t queue_capacity;
  uint32_t queue_full;  // QueueFullPolicy
  uint32_t max_held;
  uint32_t url_length;
  char url[kMaxUrlLength];
};

struct WelcomeMsg {
  MsgHeader h;
  uint32_t abi;
  uint32_t queue_capacity;
  uint32_t delivery_slots;
  uint32_t return_slots;
  uint64_t segment_size;
};

struct SegmentAddMsg {
  MsgHeader h;
  uint32_t segment_id;
  uint32_t chunk_count;
  uint64_t chunk_stride;
  uint64_t segment_size;
};

struct SegmentAckMsg {
  MsgHeader h;
  uint32_t segment_id;
};

struct RejectMsg {
  MsgHeader h;
  uint32_t reason;  // Error
};

inline constexpr size_t kMaxMsgSize = sizeof(HelloMsg);

}  // namespace ibmw::selfdev::shm::detail
