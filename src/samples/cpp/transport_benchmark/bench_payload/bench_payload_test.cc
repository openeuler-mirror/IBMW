// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Unit tests for bench_payload.h (POD path helpers only -- the
// CompressedImage helpers are templated on the IDL type and validated
// transitively by the per-sample build).

#include <cstring>
#include <vector>

#include "gtest/gtest.h"

#include "bench_payload.h"
#include "bench_plain_payloads.h"

namespace ibmw::samples::transport_benchmark {

TEST(BenchPodHeader, IsTriviallyCopyableAnd16Bytes) {
  static_assert(std::is_trivially_copyable_v<BenchPodHeader>);
  static_assert(sizeof(BenchPodHeader) == 16);
  EXPECT_EQ(sizeof(BenchPodHeader), 16u);
}

TEST(FillBenchPod, RejectsNullAndUndersizedBuffer) {
  std::vector<std::byte> buf(8);
  EXPECT_FALSE(FillBenchPod(nullptr, 100, 0));
  EXPECT_FALSE(FillBenchPod(buf.data(), buf.size(), 0));  // 8 < 16
}

TEST(FillBenchPod, FillsHeaderAndFiller) {
  std::vector<std::byte> buf(64);
  ASSERT_TRUE(FillBenchPod(buf.data(), buf.size(), 42));

  auto* hdr = reinterpret_cast<BenchPodHeader*>(buf.data());
  EXPECT_EQ(hdr->seq, 42u);
  EXPECT_EQ(hdr->size, 64u);
  EXPECT_GT(hdr->send_ns, 0u);

  // Filler region must be all kBenchFillerMagic.
  for (size_t i = sizeof(BenchPodHeader); i < buf.size(); ++i) {
    EXPECT_EQ(static_cast<uint8_t>(buf[i]), kBenchFillerMagic) << "i=" << i;
  }
}

TEST(FillBenchPod, ExactHeaderSizeNoFiller) {
  std::vector<std::byte> buf(sizeof(BenchPodHeader));
  ASSERT_TRUE(FillBenchPod(buf.data(), buf.size(), 7));
  auto* hdr = reinterpret_cast<BenchPodHeader*>(buf.data());
  EXPECT_EQ(hdr->seq, 7u);
  EXPECT_EQ(hdr->size, sizeof(BenchPodHeader));
}

TEST(ParseBenchPod, RoundTripsFillThenParse) {
  std::vector<std::byte> buf(256);
  ASSERT_TRUE(FillBenchPod(buf.data(), buf.size(), 99));

  uint64_t send_ns = 0;
  uint32_t seq = 0;
  ASSERT_TRUE(ParseBenchPod(buf.data(), buf.size(), &send_ns, &seq));
  EXPECT_EQ(seq, 99u);
  EXPECT_GT(send_ns, 0u);
}

TEST(ParseBenchPod, RejectsSizeMismatch) {
  std::vector<std::byte> buf(64);
  ASSERT_TRUE(FillBenchPod(buf.data(), buf.size(), 1));
  // Lie about the buffer size shorter than the header claims -- header
  // says 64 but we pass 32; the buffer is "smaller than promised", reject.
  uint64_t ns;
  uint32_t seq;
  EXPECT_FALSE(ParseBenchPod(buf.data(), 32, &ns, &seq));
  // Receiver buffer LARGER than the header says is OK: iceoryx may round
  // a chunk up to its mempool tier, so the wire `size` may be smaller
  // than the receive-side `size`. The header is still authoritative for
  // payload bounds.
  EXPECT_TRUE(ParseBenchPod(buf.data(), 128, &ns, &seq));
}

TEST(ParseBenchPod, RejectsNullAndUndersized) {
  uint64_t ns;
  uint32_t seq;
  EXPECT_FALSE(ParseBenchPod(nullptr, 100, &ns, &seq));
  std::vector<std::byte> tiny(8);
  EXPECT_FALSE(ParseBenchPod(tiny.data(), tiny.size(), &ns, &seq));
}

TEST(ParseBenchPod, NullOutPointersAreOk) {
  std::vector<std::byte> buf(32);
  ASSERT_TRUE(FillBenchPod(buf.data(), buf.size(), 0));
  EXPECT_TRUE(ParseBenchPod(buf.data(), buf.size(), nullptr, nullptr));
}

// ---------------------------------------------------------------------------
// bench_plain_payloads.h: type bank + dispatch + CDR specs
// ---------------------------------------------------------------------------

TEST(BenchPlainPayloads, ExactSizesAndPlainTrait) {
  EXPECT_EQ(sizeof(BenchPlainPayload1KB),   1024u);
  EXPECT_EQ(sizeof(BenchPlainPayload16KB),  16384u);
  EXPECT_EQ(sizeof(BenchPlainPayload256KB), 262144u);
  EXPECT_EQ(sizeof(BenchPlainPayload1MB),   1048576u);
  EXPECT_EQ(sizeof(BenchPlainPayload4MB),   4194304u);

  static_assert(std::is_trivially_copyable_v<BenchPlainPayload1KB>);
  static_assert(std::is_trivially_copyable_v<BenchPlainPayload16KB>);
  static_assert(BenchPlainPayload1KB::kIsPlainType);
  static_assert(BenchPlainPayload4MB::kIsPlainType);
}

TEST(BenchPlainPayloads, FillThenExtractRoundTrip) {
  BenchPlainPayload1KB msg{};
  FillBenchPlainPayload(msg, /*seq=*/123);
  EXPECT_EQ(ExtractBenchPlainSeq(msg), 123u);
  EXPECT_GT(ExtractBenchPlainSendNs(msg), 0u);
  EXPECT_EQ(msg.header.payload_bytes, 1024u);
  // seq != 0 path: filler must be magic
  EXPECT_EQ(msg.filler[0], kBenchPlainFillerMagic);
  EXPECT_EQ(msg.filler.back(), kBenchPlainFillerMagic);
}

TEST(BenchPlainPayloads, FillSeqZeroSkipsFiller) {
  BenchPlainPayload1KB msg{};
  FillBenchPlainPayload(msg, /*seq=*/0);
  // Filler stays zero-initialized for seq == 0 (warmup).
  EXPECT_EQ(msg.filler[0], 0u);
}

TEST(BenchPlainPayloads, DispatchByPayloadBytesHitsAllSizes) {
  size_t hits = 0;
  for (std::size_t n : {1024u, 16384u, 262144u, 1048576u, 4194304u}) {
    bool ok = DispatchByPayloadBytes(n, [&](auto type_tag) {
      using PlainT = typename decltype(type_tag)::type;
      EXPECT_EQ(PlainT::kPayloadBytes, n);
      EXPECT_EQ(sizeof(PlainT), n);
      ++hits;
    });
    EXPECT_TRUE(ok) << "n=" << n;
  }
  EXPECT_EQ(hits, 5u);
}

TEST(BenchPlainPayloads, DispatchByPayloadBytesRejectsUnsupported) {
  bool called = false;
  bool ok = DispatchByPayloadBytes(2048, [&](auto) { called = true; });
  EXPECT_FALSE(ok);
  EXPECT_FALSE(called);
}

TEST(BenchPlainPayloads, PayloadSizeLabelKnownAndUnknown) {
  EXPECT_STREQ(PayloadSizeLabel(1024), "1KB");
  EXPECT_STREQ(PayloadSizeLabel(4194304), "4MB");
  EXPECT_EQ(PayloadSizeLabel(2048), nullptr);
}

// Force-instantiate the FastCDR specializations + the
// MessageTypeSupportTraits<T>::Get() entry points (which inside their
// bodies reference eprosima::fastcdr::serialize/deserialize/calculate
// for each Plain type). If any spec is missing or ill-formed we fail at
// link time rather than silently drop the path.
TEST(BenchPlainPayloads, CdrSpecsAndContextTraitsLink) {
  EXPECT_NE(ibmw::MessageTypeSupportTraits<BenchPlainPayload1KB>::Get(), nullptr);
  EXPECT_NE(ibmw::MessageTypeSupportTraits<BenchPlainPayload16KB>::Get(), nullptr);
  EXPECT_NE(ibmw::MessageTypeSupportTraits<BenchPlainPayload256KB>::Get(), nullptr);
  EXPECT_NE(ibmw::MessageTypeSupportTraits<BenchPlainPayload1MB>::Get(), nullptr);
  EXPECT_NE(ibmw::MessageTypeSupportTraits<BenchPlainPayload4MB>::Get(), nullptr);

  // Drive serialize() once for the smallest type so the CDR spec is
  // actually exercised (not just the typed-erased lambda body).
  BenchPlainPayload1KB msg{};
  FillBenchPlainPayload(msg, /*seq=*/1);

  eprosima::fastcdr::CdrSizeCalculator calc(
      eprosima::fastcdr::CdrVersion::XCDRv2);
  size_t alignment = 0;
  size_t est = eprosima::fastcdr::calculate_serialized_size(calc, msg, alignment);
  // header (24 B) + filler (1000 B) -- no CDR length prefix for fixed array.
  EXPECT_GE(est, 1024u);

  std::vector<char> buf(est + 16);
  eprosima::fastcdr::FastBuffer fb(buf.data(), buf.size());
  eprosima::fastcdr::Cdr cdr(fb,
                             eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                             eprosima::fastcdr::Cdr::DDS_CDR);
  eprosima::fastcdr::serialize(cdr, msg);

  BenchPlainPayload1KB out{};
  eprosima::fastcdr::FastBuffer fb_in(buf.data(), buf.size());
  eprosima::fastcdr::Cdr cdr_in(fb_in,
                                eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                eprosima::fastcdr::Cdr::DDS_CDR);
  eprosima::fastcdr::deserialize(cdr_in, out);
  EXPECT_EQ(out.header.sequence_num, msg.header.sequence_num);
  EXPECT_EQ(out.header.payload_bytes, msg.header.payload_bytes);
  EXPECT_EQ(out.filler[0], msg.filler[0]);
}

}  // namespace ibmw::samples::transport_benchmark
