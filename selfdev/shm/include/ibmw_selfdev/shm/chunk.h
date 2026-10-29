// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

#include "ibmw_selfdev/shm/constants.h"

namespace ibmw::selfdev::shm {

inline constexpr uint32_t kChunkMagic = 0x49534843;  // "CHSI"

// Header at the start of every chunk. Written by the publisher only; the
// subscriber maps data segments read-only. The last 4 bytes of the header (or
// the 4 bytes right before the payload when alignment > 64) hold the distance
// from the payload back to the header, so a payload pointer alone is enough to
// find it (same trick as iceoryx ChunkHeader::fromUserPayload).
struct ChunkHeader {
  uint32_t magic;
  uint16_t abi;
  uint16_t reserved0;
  uint32_t segment_id;
  uint32_t chunk_index;
  uint32_t generation;
  uint32_t payload_size;
  uint32_t payload_offset;
  uint32_t payload_alignment;
  uint64_t sequence;
  uint64_t publish_timestamp_ns;
  uint8_t reserved1[12];
  uint32_t back_offset;  // only meaningful when payload_offset == 64
};
static_assert(sizeof(ChunkHeader) == kChunkHeaderSize);

// Read-only view of a chunk given its user payload pointer. O(1).
class ChunkView {
 public:
  static ChunkView FromUserPayload(const void* user_payload) noexcept;

  bool Valid() const noexcept;
  uint32_t PayloadSize() const noexcept { return header_->payload_size; }
  uint32_t PayloadAlignment() const noexcept { return header_->payload_alignment; }
  uint64_t Sequence() const noexcept { return header_->sequence; }
  uint64_t PublishTimestampNs() const noexcept { return header_->publish_timestamp_ns; }
  const ChunkHeader* Header() const noexcept { return header_; }

 private:
  explicit ChunkView(const ChunkHeader* h) noexcept : header_(h) {}
  const ChunkHeader* header_;
};

}  // namespace ibmw::selfdev::shm
