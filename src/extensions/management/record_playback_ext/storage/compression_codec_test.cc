// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <unistd.h>

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

// EXT-037: a frame header can declare any size; at most 64 MiB of it is
// reserved up front.
TEST(CompressionCodecTest, DeclaredSizeOnlyReservesUpToTheHint) {
  EXPECT_EQ(DecompressReserveHint(0, kPlainSize), 0u);
  EXPECT_EQ(DecompressReserveHint(kPlainSize, kPlainSize), kPlainSize);
  EXPECT_EQ(DecompressReserveHint(kPlainSize + 1, kPlainSize), 0u);
  EXPECT_EQ(DecompressReserveHint(kMaxDecompressedPayloadSize, kMaxDecompressedPayloadSize),
            kMaxDecompressReserveHint);
  EXPECT_EQ(DecompressReserveHint(~0ull, kMaxDecompressedPayloadSize), 0u);

  // Larger than the hint still round-trips: the output grows past it.
  const std::string plain(kMaxDecompressReserveHint + 4096, 'b');
  EXPECT_EQ(DecompressBuffer(CompressBuffer(plain, "zstd"), "zstd"), plain);
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

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

// EXT-038: the temporary file is created exclusively: a symbolic link planted
// at the old predictable name is neither followed nor kept.
TEST(CompressionCodecTest, CompressFileDoesNotFollowPlantedLink) {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("ibmw-compress-file-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  const auto input = root / "bag_0.mcap";
  const std::string plain(64 * 1024, 'm');
  std::ofstream(input, std::ios::binary) << plain;
  const auto victim = root / "victim";
  std::ofstream(victim) << "keep";
  fs::create_symlink(victim, root / "bag_0.mcap.zstd.tmp");

  const auto output = CompressFile(input, "zstd", 0);
  EXPECT_EQ(output, root / "bag_0.mcap.zstd");
  EXPECT_EQ(ReadAll(victim), "keep");
  EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(output)));
  EXPECT_EQ(DecompressBuffer(ReadAll(output), "zstd"), plain);
  EXPECT_FALSE(fs::exists(input));
  size_t entries = 0;
  for ([[maybe_unused]] const auto& entry : fs::directory_iterator(root)) ++entries;
  EXPECT_EQ(entries, 3u);  // output, victim and the planted link only
  fs::remove_all(root);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
