// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Shared payload helpers for the transport_benchmark suite.
//
// Two payload representations are supported because the six benchmark
// samples exercise two different transport paths:
//
//   1. Zero-copy POD path (iox_loaned_pod):
//        Raw byte buffer of `payload_bytes` size, with `BenchPodHeader`
//        at offset 0 and arbitrary filler in [sizeof(BenchPodHeader),
//        payload_bytes). The on-wire layout IS the in-memory layout --
//        no serialization. Both publisher and subscriber cast the iceoryx
//        chunk pointer to BenchPodHeader directly.
//
//   2. Standard IBMW pub/sub path (iox_loaned_ctx, iox_send, dds_loaned,
//      dds_send, tcp_send):
//        Reuses the existing `ibmw::protocols::sensor::CompressedImage`
//        IDL: `time_stamp` carries `MonotonicNs()` from the publisher,
//        `sequence_num` carries the message sequence, `frame_id` carries
//        the bench label, `format` is unused, and `data` is filled to
//        reach `payload_bytes` total (after CDR overhead the on-wire
//        size differs slightly, but the *user payload* is exactly
//        `payload_bytes`). This avoids introducing yet another IDL.
//
// Subscriber-side latency:
//   latency_ns = MonotonicNs() - extracted_send_ns
// Both publisher and subscriber are on the same host (a hard precondition
// for SHM/loaned transports anyway), so MonotonicNs() is coherent.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "base/benchmark_util.h"

namespace ibmw::samples::transport_benchmark {

// 16-byte header for the zero-copy POD path. Trivially copyable: safe for
// IoxRawPodPublisher (which static_asserts trivially_copyable on the
// IoxLoanedSample<std::byte> wrapper).
struct BenchPodHeader {
  uint64_t send_ns;  // MonotonicNs() at publish time
  uint32_t seq;      // message sequence (0-based)
  uint32_t size;     // total payload size in bytes (header + filler)
};
static_assert(sizeof(BenchPodHeader) == 16, "BenchPodHeader must be 16 bytes");

// Magic byte to fill the filler region. Lets the subscriber sanity-check
// that the chunk wasn't truncated/corrupted (cheap O(1) spot-check on a
// few offsets in addition to size validation).
inline constexpr uint8_t kBenchFillerMagic = 0xA5;

// Fill `buf` (`size` bytes, must be >= sizeof(BenchPodHeader)) with header
// + filler. Returns false if `size` is too small.
inline bool FillBenchPod(void* buf, size_t size, uint32_t seq) noexcept {
  if (buf == nullptr || size < sizeof(BenchPodHeader)) return false;
  auto* hdr = static_cast<BenchPodHeader*>(buf);
  hdr->send_ns = ibmw::common::util::MonotonicNs();
  hdr->seq = seq;
  hdr->size = static_cast<uint32_t>(size);
  if (size > sizeof(BenchPodHeader)) {
    std::memset(static_cast<std::byte*>(buf) + sizeof(BenchPodHeader),
                kBenchFillerMagic,
                size - sizeof(BenchPodHeader));
  }
  return true;
}

// Extract send_ns and seq from a received POD chunk. Returns false if the
// chunk is too small or the embedded `size` field disagrees with the
// chunk's actual size (catches gross corruption / mismatched run configs).
//
// Note: iceoryx may round the actual SHM chunk up to its mempool tier, so
// the wire `size` (recorded by FillBenchPod with the publisher's requested
// byte count) can be SMALLER than the receive-side `size`. We accept any
// `hdr->size` in the closed range [sizeof(BenchPodHeader), size]; outside
// that range indicates either pool corruption or a config mismatch and the
// chunk is dropped.
inline bool ParseBenchPod(const void* buf, size_t size,
                          uint64_t* out_send_ns, uint32_t* out_seq) noexcept {
  if (buf == nullptr || size < sizeof(BenchPodHeader)) return false;
  const auto* hdr = static_cast<const BenchPodHeader*>(buf);
  if (hdr->size < sizeof(BenchPodHeader) || hdr->size > size) return false;
  if (out_send_ns) *out_send_ns = hdr->send_ns;
  if (out_seq) *out_seq = hdr->seq;
  return true;
}

// ---------------------------------------------------------------------------
// CompressedImage helpers (standard pub/sub path).
// ---------------------------------------------------------------------------
//
// We declare these as free functions taking the message by reference so
// callers can include "Header.hpp" / "CompressedImage.hpp" themselves and
// pass the typed message in. Defined inline (header-only) because the
// CDR-generated types are header-only template machinery and we don't
// want bench_payload to force-link the FastDDS gencode into every TU.
//
// Convention:
//   header.time_stamp   -> MonotonicNs() at publish
//   header.sequence_num -> message sequence (0-based)
//   header.frame_id     -> bench label (e.g. "iox_send_1KB")
//   format              -> "" (unused)
//   data                -> filler bytes; resized so that data.size()
//                          + sizeof(BenchPodHeader-sized prefix) == payload
//                          (we treat header fields as "logical 16B prefix"
//                          to keep payload_bytes semantics consistent
//                          across all six samples).
//
// We approximate "user payload bytes" as `data.size()` because the IDL
// header overhead is tiny (~32 B) and constant across runs; it does not
// affect the cross-sample comparison we care about.

template <typename CompressedImageT>
inline void FillCompressedImageBench(CompressedImageT& msg,
                                     std::string_view label,
                                     uint32_t seq,
                                     size_t payload_bytes) {
  msg.format("");
  msg.header().frame_id(std::string(label));
  msg.header().sequence_num(seq);
  msg.header().time_stamp(ibmw::common::util::MonotonicNs());

  size_t data_bytes =
      payload_bytes > sizeof(BenchPodHeader)
          ? payload_bytes - sizeof(BenchPodHeader)
          : 0;
  auto& data = msg.data();
  data.resize(data_bytes);
  if (data_bytes > 0) {
    std::memset(data.data(), kBenchFillerMagic, data_bytes);
  }
}

template <typename CompressedImageT>
inline uint64_t ExtractBenchSendNs(const CompressedImageT& msg) noexcept {
  return static_cast<uint64_t>(msg.header().time_stamp());
}

template <typename CompressedImageT>
inline uint32_t ExtractBenchSeq(const CompressedImageT& msg) noexcept {
  return static_cast<uint32_t>(msg.header().sequence_num());
}

}  // namespace ibmw::samples::transport_benchmark
