// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

namespace ibmw::selfdev::shm {

// Limits kept equal to iceoryx 2.95.4 so IBMW behaviour does not change.
inline constexpr uint32_t kMaxChunksHeldPerSubscriber = 256;    // MAX_CHUNKS_HELD_PER_SUBSCRIBER_SIMULTANEOUSLY
inline constexpr uint32_t kMaxSubscriberQueueCapacity = 256;    // MAX_SUBSCRIBER_QUEUE_CAPACITY
inline constexpr uint32_t kMaxChunksAllocatedPerPublisher = 8;  // MAX_CHUNKS_ALLOCATED_PER_PUBLISHER_SIMULTANEOUSLY
inline constexpr uint32_t kDefaultPayloadAlignment = 8;         // CHUNK_DEFAULT_USER_PAYLOAD_ALIGNMENT
inline constexpr uint32_t kMaxRuntimeNameLength = 87;           // MAX_RUNTIME_NAME_LENGTH (informational)

// ibmw_shm specific limits.
inline constexpr uint32_t kAbiVersion = 1;
inline constexpr uint32_t kChunkHeaderSize = 64;
inline constexpr uint32_t kMinClassShift = 7;    // smallest payload class 128 B
inline constexpr uint32_t kMaxClassShift = 28;   // largest payload class 256 MB (LoanSerializedSample cap)
inline constexpr uint32_t kMaxSegmentsPerPublisher = 4096;  // 12-bit segment id
inline constexpr uint32_t kMaxChunksPerSegment = 1u << 20;  // 20-bit chunk index
inline constexpr uint32_t kMaxListenersPerProcess = 64;
inline constexpr uint32_t kMaxPayloadAlignment = 4096;
inline constexpr uint32_t kMaxUrlLength = 1024;

}  // namespace ibmw::selfdev::shm
