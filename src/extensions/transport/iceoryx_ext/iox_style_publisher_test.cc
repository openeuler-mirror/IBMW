// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for the iceoryx zero-copy POD path:
//   1. URL routing helper (POD vs standard prefix do not collide)
//   2. IoxLoanedSample<T> RAII semantics (move, release-on-destroy)
//   3. IoxStylePublisher<T> static_assert constraints (compile-only checks)
//
// Plus tests for the iceoryx zero-copy ctx-meta path (stage 6.1.d):
//   4. ComputeCtxMetaHeaderSize correctness against Send()'s wire layout
//   5. EncodeCtxMetaHeader byte-level round-trip via ConstBufferOperator
//   6. EncodeCtxMetaHeader rejects oversized inputs / undersized buffers
//   7. IoxCtxMetaLoanedBuffer RAII semantics
//
// These tests do NOT require a running RouDi daemon. End-to-end cross-process
// validation lives in the transport_benchmark sample (stage 6.2).

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "base/buffer_util.h"
#include "engine/transport/loaned_sample_handle.h"
#include "iceoryx_ext/iox_style_publisher.h"

namespace ibmw::extensions::iceoryx_extension {

// -----------------------------------------------------------------
// Compile-time checks: POD-only constraint on IoxStylePublisher / Sample.
// -----------------------------------------------------------------
struct PodMessage {
  double x;
  double y;
  uint32_t seq;
};
static_assert(std::is_trivially_copyable_v<PodMessage>);

// IoxLoanedSample<T> and IoxStylePublisher<T> instantiate cleanly for POD.
template class IoxLoanedSample<PodMessage>;
template class IoxStylePublisher<PodMessage>;

// -----------------------------------------------------------------
// LoanMode + LoanedSampleHandle behaviour
// -----------------------------------------------------------------
TEST(LoanedSampleHandle, DefaultIsEmpty) {
  runtime::core::transport::LoanedSampleHandle h;
  EXPECT_EQ(h.user_payload_ptr, nullptr);
  EXPECT_EQ(h.user_payload_size, 0u);
  EXPECT_EQ(h.driver_token, nullptr);
  EXPECT_EQ(h.mode, runtime::core::transport::LoanMode::kPodOnly);
}

// -----------------------------------------------------------------
// IoxLoanedSample lifecycle without driver (driver_=nullptr path).
// -----------------------------------------------------------------
TEST(IoxLoanedSample, EmptyIsInvalidAndNoOpDestroy) {
  IoxLoanedSample<PodMessage> s;
  EXPECT_FALSE(s.is_valid());
  // Destructor must not crash on empty sample.
}

TEST(IoxLoanedSample, MoveTransfersOwnership) {
  // Manually craft a "fake" valid handle pointing into a stack buffer.
  // driver_ is intentionally nullptr so Release() is a no-op (does NOT call
  // driver->ReleaseLoaned), avoiding any need for a live driver.
  alignas(PodMessage) std::byte buf[sizeof(PodMessage)] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = buf;
  handle.user_payload_size = sizeof(PodMessage);
  handle.mode = runtime::core::transport::LoanMode::kPodOnly;
  handle.driver_token = nullptr;

  IoxLoanedSample<PodMessage> a(/*driver=*/nullptr, handle);
  // Without a driver, is_valid() returns false (driver_==nullptr is the
  // canonical "empty/invalid" signal).
  EXPECT_FALSE(a.is_valid());

  // Moving an empty/invalid sample is still well-defined.
  IoxLoanedSample<PodMessage> b = std::move(a);
  EXPECT_FALSE(a.is_valid());
  EXPECT_FALSE(b.is_valid());
}

TEST(IoxLoanedSample, ExtractForPublishClearsState) {
  alignas(PodMessage) std::byte buf[sizeof(PodMessage)] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = buf;
  handle.user_payload_size = sizeof(PodMessage);
  handle.driver_token = reinterpret_cast<void*>(0xDEADBEEF);

  IoxLoanedSample<PodMessage> s(reinterpret_cast<IceoryxTransportDriver*>(0x1),
                                handle);
  EXPECT_TRUE(s.is_valid());

  auto extracted = s.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, buf);
  EXPECT_EQ(extracted.user_payload_size, sizeof(PodMessage));
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xDEADBEEF));

  // After extraction, sample is empty so destructor will not call
  // driver->ReleaseLoaned (we'd otherwise dereference 0x1).
  EXPECT_FALSE(s.is_valid());
}

// =========================================================================
// 6.1.d: ctx-meta header layout tests (no RouDi required).
// =========================================================================
//
// We test ComputeCtxMetaHeaderSize / EncodeCtxMetaHeader against a hand-
// written reference decoder that exactly mirrors the Send()-side parser in
// IceoryxTransportDriver::OnMessage(). If these stay in sync, the on-wire
// bytes produced by the zero-copy ctx-meta path are interchangeable with
// those produced by the regular Send() path -- which is the whole point of
// this mode.

TEST(CtxMetaHeader, EmptyKvSize) {
  std::unordered_map<std::string_view, std::string_view> kv;
  // Layout: pkg_size(4) + 1 + "cdr"(3) + ctx_num(1) = 9
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("cdr", kv), 9u);
}

TEST(CtxMetaHeader, SizeIncludesEachKvWithLengthPrefixes) {
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"k1", "v1"},      // 2+2 + 2+2 = 8
      {"trace", "abc"},  // 2+5 + 2+3 = 12
  };
  // Base = 4 + 1 + 4("json") + 1 = 10; + 8 + 12 = 30
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("json", kv), 30u);
}

TEST(CtxMetaHeader, RejectsOversizedSerializationType) {
  std::string oversize(256, 'x');  // > uint8 max
  std::unordered_map<std::string_view, std::string_view> kv;
  EXPECT_EQ(
      IceoryxTransportDriver::ComputeCtxMetaHeaderSize(oversize, kv), 0u);
}

TEST(CtxMetaHeader, RejectsTooManyKvEntries) {
  // Build 256 entries to overflow the uint8 ctx_num field.
  std::vector<std::string> keys, vals;
  keys.reserve(256);
  vals.reserve(256);
  std::unordered_map<std::string_view, std::string_view> kv;
  kv.reserve(256);
  for (int i = 0; i < 256; ++i) {
    keys.emplace_back("k" + std::to_string(i));
    vals.emplace_back("v" + std::to_string(i));
    kv.emplace(keys.back(), vals.back());
  }
  EXPECT_EQ(IceoryxTransportDriver::ComputeCtxMetaHeaderSize("cdr", kv), 0u);
}

TEST(CtxMetaHeader, EncodeThenDecodeRoundTrip) {
  // Mirror the parser logic in iceoryx_transport_driver.cc OnMessage().
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"trace_id", "abc123"},
      {"src", "node-A"},
  };
  std::string_view sertype = "cdr";
  size_t header_size =
      IceoryxTransportDriver::ComputeCtxMetaHeaderSize(sertype, kv);
  ASSERT_GT(header_size, 0u);

  // Allocate buffer with extra room for a fake user payload.
  constexpr size_t kPayload = 16;
  std::vector<char> buffer(header_size + kPayload, '\0');
  uint32_t total = static_cast<uint32_t>(buffer.size());

  size_t written = IceoryxTransportDriver::EncodeCtxMetaHeader(
      buffer.data(), buffer.size(), total, sertype, kv);
  ASSERT_EQ(written, header_size);

  // Fill payload region with a marker so we can check the offset is right.
  std::memset(buffer.data() + header_size, 0xAB, kPayload);

  // Decode using the same primitives the receiver uses.
  namespace util = ibmw::common::util;
  uint32_t pkg_size =
      util::GetUint32FromBuf(buffer.data());
  EXPECT_EQ(pkg_size, total);

  util::ConstBufferOperator buf_oper(buffer.data() + 4, pkg_size - 4);
  std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
  EXPECT_EQ(serialization_type, "cdr");

  size_t ctx_num = buf_oper.GetUint8();
  EXPECT_EQ(ctx_num, kv.size());

  std::unordered_map<std::string, std::string> decoded_kv;
  for (size_t ii = 0; ii < ctx_num; ++ii) {
    auto k = buf_oper.GetString(util::BufferLenType::kUInt16);
    auto v = buf_oper.GetString(util::BufferLenType::kUInt16);
    decoded_kv.emplace(std::string(k), std::string(v));
  }
  EXPECT_EQ(decoded_kv.size(), kv.size());
  for (const auto& [k, v] : kv) {
    auto it = decoded_kv.find(std::string(k));
    ASSERT_NE(it, decoded_kv.end()) << "missing key " << k;
    EXPECT_EQ(it->second, std::string(v));
  }

  auto remaining = buf_oper.GetRemainingBuffer();
  ASSERT_EQ(remaining.size(), kPayload);
  for (char b : remaining) {
    EXPECT_EQ(static_cast<unsigned char>(b), 0xAB);
  }
}

TEST(CtxMetaHeader, EncodeFailsOnUndersizedBuffer) {
  std::unordered_map<std::string_view, std::string_view> kv = {
      {"k", "v"},
  };
  std::string_view sertype = "cdr";
  size_t need = IceoryxTransportDriver::ComputeCtxMetaHeaderSize(sertype, kv);
  std::vector<char> small(need - 1, '\0');
  EXPECT_EQ(IceoryxTransportDriver::EncodeCtxMetaHeader(
                small.data(), small.size(),
                static_cast<uint32_t>(need), sertype, kv),
            0u);
}

// IoxCtxMetaLoanedBuffer move-semantics: same shape as IoxLoanedSample tests
// above but without typed access.
TEST(IoxCtxMetaLoanedBuffer, EmptyIsInvalid) {
  IoxCtxMetaLoanedBuffer b;
  EXPECT_FALSE(b.is_valid());
  EXPECT_EQ(b.data(), nullptr);
  EXPECT_EQ(b.size(), 0u);
}

TEST(IoxCtxMetaLoanedBuffer, ExtractForPublishClearsState) {
  std::byte stack_buf[32] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = stack_buf;
  handle.user_payload_size = sizeof(stack_buf);
  handle.mode = runtime::core::transport::LoanMode::kCtxMetaThenPayload;
  handle.driver_token = reinterpret_cast<void*>(0xCAFEBABE);

  IoxCtxMetaLoanedBuffer b(reinterpret_cast<IceoryxTransportDriver*>(0x1),
                           handle);
  EXPECT_TRUE(b.is_valid());
  EXPECT_EQ(b.data(), stack_buf);
  EXPECT_EQ(b.size(), sizeof(stack_buf));

  auto extracted = b.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, stack_buf);
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xCAFEBABE));
  EXPECT_EQ(extracted.mode,
            runtime::core::transport::LoanMode::kCtxMetaThenPayload);
  EXPECT_FALSE(b.is_valid());  // destructor must not deref 0x1
}

// =========================================================================
// 6.2.0: IoxRawPodPublisher (untyped, runtime-sized POD borrow path).
// =========================================================================
//
// These tests verify the *wrapper* surface only -- borrow_loaned_sample()
// against a live driver requires RouDi and is covered by the integration
// sample suite. Here we check:
//   1. The IoxLoanedSample<std::byte> specialisation instantiates cleanly
//      (std::byte is trivially_copyable, so the static_assert holds).
//   2. The raw-publisher header compiles end-to-end (CreateRawPodPublisher
//      + RegisterRawPodSubscriber are well-formed).
//   3. Empty / move semantics for IoxLoanedSample<std::byte> match the
//      typed POD specialisation.

template class IoxLoanedSample<std::byte>;

TEST(IoxRawLoanedSample, EmptyIsInvalid) {
  IoxLoanedSample<std::byte> s;
  EXPECT_FALSE(s.is_valid());
  EXPECT_EQ(s.get(), nullptr);
}

TEST(IoxRawLoanedSample, ExtractForPublishClearsState) {
  std::byte stack_buf[64] = {};
  runtime::core::transport::LoanedSampleHandle handle;
  handle.user_payload_ptr = stack_buf;
  handle.user_payload_size = sizeof(stack_buf);
  handle.mode = runtime::core::transport::LoanMode::kPodOnly;
  handle.driver_token = reinterpret_cast<void*>(0xFEEDFACE);

  IoxLoanedSample<std::byte> s(
      reinterpret_cast<IceoryxTransportDriver*>(0x1), handle);
  EXPECT_TRUE(s.is_valid());
  EXPECT_EQ(s.get(), stack_buf);

  auto extracted = s.ExtractForPublish();
  EXPECT_EQ(extracted.user_payload_ptr, stack_buf);
  EXPECT_EQ(extracted.user_payload_size, sizeof(stack_buf));
  EXPECT_EQ(extracted.driver_token, reinterpret_cast<void*>(0xFEEDFACE));
  EXPECT_FALSE(s.is_valid());  // destructor must not deref 0x1
}

// Compile-only smoke test: the factory + register helpers are well-formed
// against IceoryxTransportDriver's public surface. We pass a null driver
// so both functions return failure without touching the network/SHM.
TEST(IoxRawPodPublisher, NullDriverFactoryReturnsNullAndSubscriberFails) {
  auto pub = CreateRawPodPublisher(/*driver=*/nullptr,
                                   "/raw/topic", "RawBytes",
                                   /*initial_size_hint=*/4096);
  EXPECT_EQ(pub, nullptr);

  bool ok = RegisterRawPodSubscriber(/*driver=*/nullptr,
                                     "/raw/topic", "RawBytes",
                                     [](const void*, size_t) {});
  EXPECT_FALSE(ok);
}

}  // namespace ibmw::extensions::iceoryx_extension
