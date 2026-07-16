// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Mirror of bench_plain_payloads.h for the dds_send sample, intentionally
// NOT opted into the PlainDdsPubSubType<T> path.
//
// Why a separate type bank?
//   IBMW_DDS_PLAIN_TYPE(T) specialises a global trait
//   (IsPlainDdsType<T>::value -> true). The dds_type_support.h custom
//   factory consults the trait via `if constexpr` and -- once specialised --
//   ALL TUs in the process see PlainDdsPubSubType<T> for that type, which
//   sets is_plain()==true and lets FastDDS engage DataSharing/SHM
//   zero-copy. That is exactly what dds_loaned wants.
//
//   But for the dds_send sample the matrix entry is supposed to measure
//   the "DDS standard publish path" -- FastCDR serialise + transport
//   in-process copy -- so we WANT GenericDdsPubSubType<T> with
//   is_plain()==false to avoid DataSharing entirely. The cleanest way
//   to keep both behaviours co-existing in the same process tree is
//   distinct C++ types: a parallel BenchSendPayload<N> bank that omits
//   the IBMW_DDS_PLAIN_TYPE specialisation.
//
// Apart from the namespace + DDS type-name suffix + macro names, the
// layout, CDR encoding, fillers, dispatcher and label semantics are
// identical to bench_plain_payloads.h. This guarantees an apples-to-apples
// payload comparison between dds_send and the other 5 bench samples.
//
// Topic naming: dds_send uses topic `dds_bench_send_topic` on
// domain_id=43, which is already isolated from dds_loaned's
// `dds_bench_topic` on domain_id=42. The DDS type-name suffix
// `_NonPlain` is an additional belt-and-braces guard against any
// PubSubType registry collision inside FastDDS.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#include "dds_types/util/fastcdr_v1_compat.h"
#include "dds_types/util/dds_type_support.h"

#include "base/benchmark_util.h"

namespace ibmw::samples::transport_benchmark::dds_send_bank {

// 24 bytes -- multiple of 8, matches BenchPlainHeader for byte-equivalence.
struct BenchSendHeader {
  uint64_t send_ns = 0;
  uint64_t sequence_num = 0;
  uint64_t payload_bytes = 0;
};
static_assert(sizeof(BenchSendHeader) == 24,
              "BenchSendHeader must be 24 bytes for natural alignment");
static_assert(std::is_trivially_copyable_v<BenchSendHeader>);

inline constexpr uint8_t kBenchSendFillerMagic = 0xA5;

template <std::size_t N>
struct BenchSendPayload {
  static_assert(N >= sizeof(BenchSendHeader),
                "BenchSendPayload N must be >= sizeof(BenchSendHeader)");
  static_assert((N % 8) == 0,
                "BenchSendPayload N must be multiple of 8");

  static constexpr std::size_t kPayloadBytes = N;
  static constexpr std::size_t kFillerBytes = N - sizeof(BenchSendHeader);

  BenchSendHeader header{};
  std::array<uint8_t, kFillerBytes> filler{};
};

static_assert(sizeof(BenchSendPayload<1024>) == 1024);
static_assert(sizeof(BenchSendPayload<4096>) == 4096);
static_assert(sizeof(BenchSendPayload<16384>) == 16384);
static_assert(sizeof(BenchSendPayload<65536>) == 65536);
static_assert(sizeof(BenchSendPayload<262144>) == 262144);
static_assert(sizeof(BenchSendPayload<1048576>) == 1048576);
static_assert(sizeof(BenchSendPayload<4194304>) == 4194304);

using BenchSendPayload1KB   = BenchSendPayload<1024>;
using BenchSendPayload4KB   = BenchSendPayload<4096>;
using BenchSendPayload16KB  = BenchSendPayload<16384>;
using BenchSendPayload64KB  = BenchSendPayload<65536>;
using BenchSendPayload256KB = BenchSendPayload<262144>;
using BenchSendPayload1MB   = BenchSendPayload<1048576>;
using BenchSendPayload4MB   = BenchSendPayload<4194304>;

#define IBMW_BENCH_SEND_FOR_EACH(X)                                                  \
  X("1KB",   1024,    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1KB)   \
  X("4KB",   4096,    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4KB)   \
  X("16KB",  16384,   ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload16KB)  \
  X("64KB",  65536,   ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload64KB)  \
  X("256KB", 262144,  ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload256KB) \
  X("1MB",   1048576, ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1MB)   \
  X("4MB",   4194304, ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4MB)

template <typename SendT>
inline void FillBenchSendPayload(SendT& msg, uint32_t seq) noexcept {
  msg.header.send_ns = ibmw::common::util::MonotonicNs();
  msg.header.sequence_num = seq;
  msg.header.payload_bytes = SendT::kPayloadBytes;
  if (seq != 0) {
    std::memset(msg.filler.data(), kBenchSendFillerMagic, msg.filler.size());
  }
}

template <typename SendT>
inline uint64_t ExtractBenchSendNs(const SendT& msg) noexcept {
  return msg.header.send_ns;
}

template <typename SendT>
inline uint32_t ExtractBenchSendSeq(const SendT& msg) noexcept {
  return static_cast<uint32_t>(msg.header.sequence_num);
}

template <typename T>
struct SendTypeTag { using type = T; };

template <typename F>
inline bool DispatchSendByPayloadBytes(std::size_t payload_bytes, F&& fn) {
#define IBMW_BENCH_SEND_DISPATCH_CASE(LBL, N, ALIAS)   \
  if (payload_bytes == (N)) {                           \
    fn(SendTypeTag<ALIAS>{});                           \
    return true;                                        \
  }
  IBMW_BENCH_SEND_FOR_EACH(IBMW_BENCH_SEND_DISPATCH_CASE)
#undef IBMW_BENCH_SEND_DISPATCH_CASE
  return false;
}

inline const char* SendPayloadSizeLabel(std::size_t payload_bytes) noexcept {
#define IBMW_BENCH_SEND_LABEL_CASE(LBL, N, ALIAS) if (payload_bytes == (N)) return LBL;
  IBMW_BENCH_SEND_FOR_EACH(IBMW_BENCH_SEND_LABEL_CASE)
#undef IBMW_BENCH_SEND_LABEL_CASE
  return nullptr;
}

}  // namespace ibmw::samples::transport_benchmark::dds_send_bank

// ---------------------------------------------------------------------------
// FastCDR v1 inline serialize / deserialize / calculate_serialized_size
// ---------------------------------------------------------------------------

namespace eprosima {
namespace fastcdr {

#define IBMW_BENCH_SEND_CDR_SPECS(LBL, N, ALIAS)                               \
  template <>                                                                  \
  inline size_t calculate_serialized_size(                                     \
      CdrSizeCalculator& calculator,                                           \
      const ALIAS& data,                                                       \
      size_t& current_alignment) {                                             \
    size_t total = 0;                                                          \
    total += calculator.calculate_member_serialized_size(                      \
        MemberId(0), data.header.send_ns, current_alignment);                  \
    total += calculator.calculate_member_serialized_size(                      \
        MemberId(1), data.header.sequence_num, current_alignment);             \
    total += calculator.calculate_member_serialized_size(                      \
        MemberId(2), data.header.payload_bytes, current_alignment);            \
    total += calculator.calculate_member_serialized_size(                      \
        MemberId(3), data.filler, current_alignment);                          \
    return total;                                                              \
  }                                                                            \
  template <>                                                                  \
  inline void serialize(Cdr& scdr, const ALIAS& data) {                        \
    scdr << data.header.send_ns                                                \
         << data.header.sequence_num                                           \
         << data.header.payload_bytes                                          \
         << data.filler;                                                       \
  }                                                                            \
  template <>                                                                  \
  inline void deserialize(Cdr& cdr, ALIAS& data) {                             \
    cdr >> data.header.send_ns                                                 \
        >> data.header.sequence_num                                            \
        >> data.header.payload_bytes                                           \
        >> data.filler;                                                        \
  }

IBMW_BENCH_SEND_FOR_EACH(IBMW_BENCH_SEND_CDR_SPECS)
#undef IBMW_BENCH_SEND_CDR_SPECS

}  // namespace fastcdr
}  // namespace eprosima

// ---------------------------------------------------------------------------
// IBMW DDS type-name + Context API trait registrations.
//
// Crucially, NO IBMW_DDS_PLAIN_TYPE specialisation. The dds_type_support
// factory therefore picks GenericDdsPubSubType<T> -> is_plain()==false ->
// FastDDS skips DataSharing/SHM and uses the standard intra-process
// transport (FastCDR serialise + buffer copy).
// ---------------------------------------------------------------------------

IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1KB,
    "ibmw::samples::transport_benchmark::BenchSendPayload1KB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4KB,
    "ibmw::samples::transport_benchmark::BenchSendPayload4KB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload16KB,
    "ibmw::samples::transport_benchmark::BenchSendPayload16KB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload64KB,
    "ibmw::samples::transport_benchmark::BenchSendPayload64KB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload256KB,
    "ibmw::samples::transport_benchmark::BenchSendPayload256KB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1MB,
    "ibmw::samples::transport_benchmark::BenchSendPayload1MB_NonPlain")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4MB,
    "ibmw::samples::transport_benchmark::BenchSendPayload4MB_NonPlain")

#include "dds_types/ibmw_dds_types.h"
#include "cpp/context/details/type_support.h"

IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload16KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload64KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload256KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload1MB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::dds_send_bank::BenchSendPayload4MB)
