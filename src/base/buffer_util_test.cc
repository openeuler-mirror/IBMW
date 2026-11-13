// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for src/base/buffer_util.h
//   - Bounds checks must reject lengths beyond the remaining bytes, including a
//     wire-supplied uint64 string length that would wrap the cursor
//   - GetLengthPrefixedPayload() must not trust the wire length prefix

#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "base/buffer_util.h"

namespace ibmw::common::util {

namespace {

std::vector<char> MakeFrame(uint32_t declared_len, size_t payload_len) {
  std::vector<char> buf(4 + payload_len, 'x');
  BufferOperator op(buf.data(), buf.size());
  op.SetUint32(declared_len);
  return buf;
}

}  // namespace

TEST(GetLengthPrefixedPayload, ReturnsExactlyDeclaredPayload) {
  auto buf = MakeFrame(3, 5);  // trailing bytes beyond the declared length are ignored
  auto payload = GetLengthPrefixedPayload(buf.data(), buf.size());
  EXPECT_EQ(payload.data(), buf.data() + 4);
  EXPECT_EQ(payload.size(), 3u);
}

TEST(GetLengthPrefixedPayload, AcceptsEmptyPayload) {
  auto buf = MakeFrame(0, 0);
  EXPECT_EQ(GetLengthPrefixedPayload(buf.data(), buf.size()).size(), 0u);
}

TEST(GetLengthPrefixedPayload, RejectsBufferShorterThanPrefix) {
  std::vector<char> buf(3, '\0');
  EXPECT_THROW(GetLengthPrefixedPayload(buf.data(), buf.size()), BufferOutOfBoundsException);
  EXPECT_THROW(GetLengthPrefixedPayload(nullptr, 0), BufferOutOfBoundsException);
}

TEST(GetLengthPrefixedPayload, RejectsDeclaredLengthBeyondReceivedBytes) {
  auto buf = MakeFrame(6, 5);
  EXPECT_THROW(GetLengthPrefixedPayload(buf.data(), buf.size()), BufferOutOfBoundsException);
  auto huge = MakeFrame(std::numeric_limits<uint32_t>::max(), 8);
  EXPECT_THROW(GetLengthPrefixedPayload(huge.data(), huge.size()), BufferOutOfBoundsException);
}

TEST(ConstBufferOperator, GetStringRejectsWrappingLength) {
  // A uint64 length close to SIZE_MAX used to make `cur_ + len` wrap and pass the check.
  std::vector<char> buf(16, '\0');
  BufferOperator w(buf.data(), buf.size());
  w.SetUint64(std::numeric_limits<uint64_t>::max() - 4);
  ConstBufferOperator r(buf.data(), buf.size());
  EXPECT_THROW(r.GetString(BufferLenType::kUInt64), BufferOutOfBoundsException);
}

TEST(ConstBufferOperator, GetStringRejectsUInt64LengthBeyondRemaining) {
  std::vector<char> buf(16, '\0');
  BufferOperator w(buf.data(), buf.size());
  w.SetUint64(9);  // 8 bytes remain after the prefix
  ConstBufferOperator r(buf.data(), buf.size());
  EXPECT_THROW(r.GetString(BufferLenType::kUInt64), BufferOutOfBoundsException);
}

TEST(ConstBufferOperator, SkipAndGetBufferRejectOverrun) {
  std::vector<char> buf(8, '\0');
  ConstBufferOperator r(buf.data(), buf.size());
  EXPECT_THROW(r.Skip(9), BufferOutOfBoundsException);
  EXPECT_THROW(r.GetBuffer(9), BufferOutOfBoundsException);
  char dst[9];
  EXPECT_THROW(r.GetBuffer(dst, 9), BufferOutOfBoundsException);
  EXPECT_EQ(r.Skip(8), 8u);
  EXPECT_THROW(r.GetUint8(), BufferOutOfBoundsException);
}

TEST(BufferOperator, RejectsOverrun) {
  std::vector<char> buf(8, '\0');
  BufferOperator w(buf.data(), buf.size());
  EXPECT_THROW(w.Skip(9), BufferOutOfBoundsException);
  EXPECT_THROW(w.SetBuffer(buf.data(), 9), BufferOutOfBoundsException);
  EXPECT_THROW(w.SetString("12345678", BufferLenType::kUInt8), BufferOutOfBoundsException);
  w.SetUint64(1);
  EXPECT_EQ(w.GetRemainingSize(), 0u);
  EXPECT_THROW(w.SetUint8(1), BufferOutOfBoundsException);
}

TEST(BufferOperator, RoundTrip) {
  std::vector<char> buf(64, '\0');
  BufferOperator w(buf.data(), buf.size());
  w.SetUint8(7);
  w.SetUint16(0x1234);
  w.SetUint32(0xdeadbeef);
  w.SetString("abc", BufferLenType::kUInt16);
  ConstBufferOperator r(buf.data(), buf.size() - w.GetRemainingSize());
  EXPECT_EQ(r.GetUint8(), 7);
  EXPECT_EQ(r.GetUint16(), 0x1234);
  EXPECT_EQ(r.GetUint32(), 0xdeadbeefu);
  EXPECT_EQ(r.GetString(BufferLenType::kUInt16), "abc");
  EXPECT_EQ(r.GetRemainingSize(), 0u);
}

}  // namespace ibmw::common::util
