// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>

#include <lz4frame.h>
#include <zstd.h>

#include "record_playback_ext/storage/compression_codec.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

constexpr size_t kPlainSize = 2 * 1024 * 1024;

// Frames without a declared content size exercise the unreserved path.
std::string ZstdWithoutContentSize(const std::string& input) {
  ZSTD_CCtx* context = ZSTD_createCCtx();
  ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0);
  std::string output(ZSTD_compressBound(input.size()), '\0');
  const size_t size = ZSTD_compress2(
      context, output.data(), output.size(), input.data(), input.size());
  ZSTD_freeCCtx(context);
  EXPECT_FALSE(ZSTD_isError(size));
  output.resize(size);
  return output;
}

std::string Lz4WithoutContentSize(const std::string& input) {
  LZ4F_preferences_t preferences{};
  std::string output(LZ4F_compressFrameBound(input.size(), &preferences), '\0');
  const size_t size = LZ4F_compressFrame(
      output.data(), output.size(), input.data(), input.size(), &preferences);
  EXPECT_FALSE(LZ4F_isError(size));
  output.resize(size);
  return output;
}

TEST(CompressionCodecTest, RoundTripWithinLimit) {
  const std::string plain(kPlainSize, 'a');
  for (const char* format : {"zstd", "lz4"}) {
    EXPECT_EQ(
        DecompressBuffer(CompressBuffer(plain, format), format, kPlainSize),
        plain)
        << format;
  }
  EXPECT_EQ(
      DecompressBuffer(ZstdWithoutContentSize(plain), "zstd", kPlainSize),
      plain);
  EXPECT_EQ(
      DecompressBuffer(Lz4WithoutContentSize(plain), "lz4", kPlainSize),
      plain);
}

TEST(CompressionCodecTest, OutputLargerThanLimitIsRejected) {
  const std::string plain(kPlainSize, '\0');
  for (const char* format : {"zstd", "lz4"}) {
    EXPECT_ANY_THROW(
        DecompressBuffer(CompressBuffer(plain, format), format, kPlainSize - 1))
        << format;
  }
  EXPECT_ANY_THROW(
      DecompressBuffer(ZstdWithoutContentSize(plain), "zstd", kPlainSize - 1));
  EXPECT_ANY_THROW(
      DecompressBuffer(Lz4WithoutContentSize(plain), "lz4", kPlainSize - 1));
}

TEST(CompressionCodecTest, CorruptedFramesAreRejected) {
  const std::string plain(4096, 'x');
  for (const char* format : {"zstd", "lz4"}) {
    const auto compressed = CompressBuffer(plain, format);
    EXPECT_ANY_THROW(DecompressBuffer(
        std::string_view(compressed).substr(0, compressed.size() / 2), format))
        << format;
    EXPECT_ANY_THROW(DecompressBuffer(std::string_view(), format)) << format;
  }
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
