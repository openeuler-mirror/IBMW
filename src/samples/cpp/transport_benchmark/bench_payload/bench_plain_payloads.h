// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Plain (POD-like, fixed-size) bench payload type bank.
//
// Why a bank of distinct types instead of one variable-size message?
//   The DDS extension's LoanedMessage zero-copy path (dds_ext) only
//   engages for "Plain" types -- types that satisfy
//   `is_plain_top_level_data` per the FastDDS XCDR2 inspector. In
//   practice that means: trivially-copyable, no string, no sequence,
//   no optional. Only fixed-size POD with fixed-size arrays qualify.
//
//   To exercise BOTH the DDS loaned (Plain-only) path AND the standard
//   serialize/heap/memcpy paths over a benchmark size matrix
//   (1KB / 16KB / 256KB / 1MB / 4MB) we therefore need one Plain type
//   per matrix row.
//
// Layout of each `BenchPlainPayload<N>`:
//
//     +--- BenchPlainHeader (24 B, all uint64_t for natural alignment) ---+
//     | send_ns         (uint64_t, MonotonicNs() at publish)              |
//     | sequence_num    (uint64_t)                                        |
//     | payload_bytes   (uint64_t, recorded N for sub-side validation)    |
//     +--- filler (N - 24 bytes) -----------------------------------------+
//     | uint8_t array, filled with kBenchFillerMagic (0xA5)               |
//     +-------------------------------------------------------------------+
//
//     sizeof(BenchPlainPayload<N>) == N exactly.
//
// CDR encoding:
//   The struct is CDR-serialized using the inline
//   `eprosima::fastcdr::serialize / deserialize / calculate_serialized_size`
//   specializations defined in this header (mirrors dds_image_demo's
//   ImageMsg recipe). Even for the iceoryx loaned-ctx path the message
//   is CDR-serialized into the loaned chunk; the CDR overhead for
//   `std::array<uint8_t, M>` is M bytes (no length prefix because the
//   array bound is known at compile time), so the on-wire size is just
//   sizeof(struct) + 4 B encapsulation header.
//
// IBMW Context API + DDS Transport registration:
//   `IBMW_DDS_TYPE_NAME` makes the type discoverable by the DDS
//   TypeSupport factory; `IBMW_DDS_REGISTER_CONTEXT_TYPE` plugs
//   `MessageTypeSupportTraits<T>` and `MessagePublisherTraits<T>` so
//   `RegisterSendType<T>` / `Send(pub,msg)` / `OnMessage<T>(...)` all
//   compile.
//
// Static-fan-out dispatch:
//   IBMW_BENCH_PLAIN_FOR_EACH(X) is an X-macro listing the (label, N)
//   tuples; modules use it to register all 5 types in Setup() and to
//   dispatch publish-side by `payload_bytes` from the yaml. See
//   `DispatchByPayloadBytes()` below.

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

namespace ibmw::samples::transport_benchmark {

// 24 bytes -- multiple of 8 so the trailing filler stays naturally
// aligned and Plain trait survives.
struct BenchPlainHeader {
  uint64_t send_ns = 0;
  uint64_t sequence_num = 0;
  uint64_t payload_bytes = 0;
};
static_assert(sizeof(BenchPlainHeader) == 24,
              "BenchPlainHeader must be 24 bytes for natural alignment");
static_assert(std::is_trivially_copyable_v<BenchPlainHeader>);

inline constexpr uint8_t kBenchPlainFillerMagic = 0xA5;

template <std::size_t N>
struct BenchPlainPayload {
  static_assert(N >= sizeof(BenchPlainHeader),
                "BenchPlainPayload N must be >= sizeof(BenchPlainHeader)");
  static_assert((N % 8) == 0,
                "BenchPlainPayload N must be multiple of 8 to stay Plain");

  // Required by the dds_ext loan path (inspected by IsPlainType()).
  static constexpr bool kIsPlainType = true;
  static constexpr std::size_t kPayloadBytes = N;
  static constexpr std::size_t kFillerBytes = N - sizeof(BenchPlainHeader);

  BenchPlainHeader header{};
  std::array<uint8_t, kFillerBytes> filler{};
};

// Compile-time sizes are exact -- this is what makes the type Plain.
static_assert(sizeof(BenchPlainPayload<1024>) == 1024);
static_assert(sizeof(BenchPlainPayload<4096>) == 4096);
static_assert(sizeof(BenchPlainPayload<16384>) == 16384);
static_assert(sizeof(BenchPlainPayload<65536>) == 65536);
static_assert(sizeof(BenchPlainPayload<262144>) == 262144);
static_assert(sizeof(BenchPlainPayload<1048576>) == 1048576);
static_assert(sizeof(BenchPlainPayload<4194304>) == 4194304);

static_assert(std::is_trivially_copyable_v<BenchPlainPayload<1024>>);

// Public aliases -- the typed traits below are specialized on these.
using BenchPlainPayload1KB   = BenchPlainPayload<1024>;
using BenchPlainPayload4KB   = BenchPlainPayload<4096>;
using BenchPlainPayload16KB  = BenchPlainPayload<16384>;
using BenchPlainPayload64KB  = BenchPlainPayload<65536>;
using BenchPlainPayload256KB = BenchPlainPayload<262144>;
using BenchPlainPayload1MB   = BenchPlainPayload<1048576>;
using BenchPlainPayload4MB   = BenchPlainPayload<4194304>;

// X-macro for static fan-out: X(LabelSuffix, N, Alias).
//   LabelSuffix: "1KB", "4KB", "16KB", ... (used to compose label_<size> strings)
//   N:           the byte count
//   Alias:       the matching type alias
#define IBMW_BENCH_PLAIN_FOR_EACH(X)                              \
  X("1KB",   1024,    ibmw::samples::transport_benchmark::BenchPlainPayload1KB)   \
  X("4KB",   4096,    ibmw::samples::transport_benchmark::BenchPlainPayload4KB)   \
  X("16KB",  16384,   ibmw::samples::transport_benchmark::BenchPlainPayload16KB)  \
  X("64KB",  65536,   ibmw::samples::transport_benchmark::BenchPlainPayload64KB)  \
  X("256KB", 262144,  ibmw::samples::transport_benchmark::BenchPlainPayload256KB) \
  X("1MB",   1048576, ibmw::samples::transport_benchmark::BenchPlainPayload1MB)   \
  X("4MB",   4194304, ibmw::samples::transport_benchmark::BenchPlainPayload4MB)

// ---------------------------------------------------------------------------
// Fill / extract helpers
// ---------------------------------------------------------------------------

template <typename PlainT>
inline void FillBenchPlainPayload(PlainT& msg, uint32_t seq) noexcept {
  msg.header.send_ns = ibmw::common::util::MonotonicNs();
  msg.header.sequence_num = seq;
  msg.header.payload_bytes = PlainT::kPayloadBytes;
  // Filler memset is a publisher-side cost we DO want to measure (it
  // mirrors the realistic case of producers actually writing user data
  // into the loaned chunk). Skip it only for the very first publish to
  // keep cold-cache effects out of warmup.
  if (seq != 0) {
    std::memset(msg.filler.data(), kBenchPlainFillerMagic, msg.filler.size());
  }
}

template <typename PlainT>
inline uint64_t ExtractBenchPlainSendNs(const PlainT& msg) noexcept {
  return msg.header.send_ns;
}

template <typename PlainT>
inline uint32_t ExtractBenchPlainSeq(const PlainT& msg) noexcept {
  return static_cast<uint32_t>(msg.header.sequence_num);
}

// ---------------------------------------------------------------------------
// Static fan-out dispatcher
// ---------------------------------------------------------------------------
//
// Modules call `DispatchByPayloadBytes(opts_.payload_bytes, fn)` where
// `fn` is a generic lambda with `template <typename PlainT> void
// operator()()`. Returns false iff `payload_bytes` is not in the
// supported matrix.
//
// Example:
//   bool ok = DispatchByPayloadBytes(opts_.payload_bytes,
//       [&](auto type_tag) {
//         using PlainT = typename decltype(type_tag)::type;
//         PlainT msg;
//         FillBenchPlainPayload(msg, seq);
//         ibmw::transport::Send(publisher_<PlainT>, msg);
//       });
//
// The `type_tag` carries the chosen type via `TypeTag<PlainT>::type`.

template <typename T>
struct TypeTag { using type = T; };

template <typename F>
inline bool DispatchByPayloadBytes(std::size_t payload_bytes, F&& fn) {
#define IBMW_BENCH_DISPATCH_CASE(LBL, N, ALIAS)        \
  if (payload_bytes == (N)) {                           \
    fn(TypeTag<ALIAS>{});                               \
    return true;                                        \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_BENCH_DISPATCH_CASE)
#undef IBMW_BENCH_DISPATCH_CASE
  return false;
}

// Static label suffix for a payload size (returns nullptr if unsupported).
inline const char* PayloadSizeLabel(std::size_t payload_bytes) noexcept {
#define IBMW_BENCH_LABEL_CASE(LBL, N, ALIAS) if (payload_bytes == (N)) return LBL;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_BENCH_LABEL_CASE)
#undef IBMW_BENCH_LABEL_CASE
  return nullptr;
}

}  // namespace ibmw::samples::transport_benchmark

// ---------------------------------------------------------------------------
// FastCDR v1 inline serialize / deserialize / calculate_serialized_size
// specializations (mirrors dds_image_demo/ImageMsg recipe).
// ---------------------------------------------------------------------------

namespace eprosima {
namespace fastcdr {

#define IBMW_BENCH_PLAIN_CDR_SPECS(LBL, N, ALIAS)                              \
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

IBMW_BENCH_PLAIN_FOR_EACH(IBMW_BENCH_PLAIN_CDR_SPECS)
#undef IBMW_BENCH_PLAIN_CDR_SPECS

}  // namespace fastcdr
}  // namespace eprosima

// ---------------------------------------------------------------------------
// IBMW DDS type-name + Context API trait registrations.
//
// Each macro must sit at namespace scope (they expand to specializations
// in ibmw::dds / ibmw / ibmw::transport).
// ---------------------------------------------------------------------------

IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload1KB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload1KB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload4KB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload4KB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload16KB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload16KB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload64KB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload64KB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload256KB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload256KB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload1MB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload1MB")
IBMW_DDS_TYPE_NAME(
    ibmw::samples::transport_benchmark::BenchPlainPayload4MB,
    "ibmw::samples::transport_benchmark::BenchPlainPayload4MB")

// MUST be after the FastCDR specializations (the macro expansion calls
// GetDdsMessageTypeSupport<T>(), which requires `serialize` /
// `deserialize` / `calculate_serialized_size` to already be visible).
// Pull in the umbrella that defines IBMW_DDS_REGISTER_CONTEXT_TYPE and
// the Context API traits header.
#include "dds_types/ibmw_dds_types.h"
#include "cpp/context/details/type_support.h"

// Opt the BenchPlainPayload types into the PlainDdsPubSubType<T> path
// so FastDDS engages LoanedMessage / DataSharing zero-copy SHM transport
// (see plain_dds_pubsub_type.h header comment for the full rationale).
// MUST come before the IBMW_DDS_REGISTER_CONTEXT_TYPE block: the trait
// is consulted from inside GetDdsMessageTypeSupport<T>()'s
// custom_type_support_ptr lambda, which is materialised on first use.
// Declaring the trait specialization first keeps that resolution
// unambiguous regardless of TU ordering.
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload1KB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload4KB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload16KB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload64KB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload256KB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload1MB)
IBMW_DDS_PLAIN_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload4MB)

IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload1KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload4KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload16KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload64KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload256KB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload1MB)
IBMW_DDS_REGISTER_CONTEXT_TYPE(ibmw::samples::transport_benchmark::BenchPlainPayload4MB)
