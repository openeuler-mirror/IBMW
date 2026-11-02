// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/compression_codec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

#include <lz4frame.h>
#include <zstd.h>

#include <fcntl.h>
#include <unistd.h>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

constexpr size_t kStreamBufferSize = 64 * 1024;

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  MW_ASSERT(input.is_open(), "Cannot open '{}' for compression.", path.string());
  const auto end = input.tellg();
  MW_ASSERT(end >= 0, "Cannot determine size of '{}'.", path.string());
  MW_ASSERT(
      static_cast<uint64_t>(end) <=
          static_cast<uint64_t>(std::numeric_limits<size_t>::max()),
      "File '{}' is too large to compress.", path.string());
  std::string data(static_cast<size_t>(end), '\0');
  input.seekg(0);
  input.read(data.data(), static_cast<std::streamsize>(data.size()));
  MW_ASSERT(
      input.good() || input.eof(), "Cannot read '{}'.", path.string());
  MW_ASSERT(
      static_cast<size_t>(input.gcount()) == data.size(),
      "Short read from '{}'.", path.string());
  return data;
}

// Closes a file descriptor and removes the temporary file unless released.
struct TemporaryFile {
  int fd = -1;
  std::string path;
  ~TemporaryFile() {
    if (fd >= 0) ::close(fd);
    if (!path.empty()) ::unlink(path.c_str());
  }
};

// A new file next to path: O_EXCL neither follows nor reuses a planted link.
TemporaryFile CreateTemporaryFile(const std::filesystem::path& path) {
  static std::atomic<uint64_t> counter{0};
  TemporaryFile file;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::string candidate = path.string() + ".tmp." + std::to_string(::getpid()) + "." +
                            std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    file.fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (file.fd >= 0) {
      file.path = std::move(candidate);
      return file;
    }
    MW_ASSERT(errno == EEXIST, "Cannot create '{}': {}.", candidate, std::strerror(errno));
  }
  MW_ASSERT(false, "Cannot create a temporary file for '{}'.", path.string());
  return file;
}

void WriteAll(const TemporaryFile& file, std::string_view data) {
  while (!data.empty()) {
    const ssize_t written = ::write(file.fd, data.data(), data.size());
    if (written < 0 && errno == EINTR) continue;
    MW_ASSERT(written > 0, "Cannot write compressed file '{}': {}.", file.path, std::strerror(errno));
    data.remove_prefix(static_cast<size_t>(written));
  }
}

void SyncDirectory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  MW_ASSERT(fd >= 0, "Cannot open directory '{}': {}.", directory.string(), std::strerror(errno));
  const int result = ::fsync(fd);
  const int error = errno;
  ::close(fd);
  MW_ASSERT(result == 0, "Cannot sync directory '{}': {}.", directory.string(), std::strerror(error));
}

std::string CompressZstd(std::string_view input, int compression_level) {
  const size_t bound = ZSTD_compressBound(input.size());
  std::string output(bound, '\0');
  const int level = compression_level == 0 ? ZSTD_CLEVEL_DEFAULT
                                            : compression_level;
  const size_t size = ZSTD_compress(
      output.data(), output.size(), input.data(), input.size(), level);
  MW_ASSERT(
      !ZSTD_isError(size), "Zstd compression failed: {}.",
      ZSTD_getErrorName(size));
  output.resize(size);
  return output;
}

std::string DecompressZstd(std::string_view input, size_t max_output_size) {
  ZSTD_DStream* stream = ZSTD_createDStream();
  MW_ASSERT(stream != nullptr, "Cannot create a Zstd decompressor.");
  struct Guard {
    ZSTD_DStream* stream;
    ~Guard() { ZSTD_freeDStream(stream); }
  } guard{stream};
  const size_t initialized = ZSTD_initDStream(stream);
  MW_ASSERT(
      !ZSTD_isError(initialized), "Cannot initialize Zstd decompression: {}.",
      ZSTD_getErrorName(initialized));

  ZSTD_inBuffer source{input.data(), input.size(), 0};
  std::array<char, kStreamBufferSize> buffer{};
  std::string output;
  // The declared size is only a reservation hint; the cap below still holds.
  const unsigned long long declared =
      ZSTD_getFrameContentSize(input.data(), input.size());
  if (declared != ZSTD_CONTENTSIZE_UNKNOWN &&
      declared != ZSTD_CONTENTSIZE_ERROR) {
    output.reserve(DecompressReserveHint(declared, max_output_size));
  }
  size_t remaining = 1;
  while (source.pos < source.size || remaining != 0) {
    ZSTD_outBuffer destination{buffer.data(), buffer.size(), 0};
    remaining = ZSTD_decompressStream(stream, &destination, &source);
    MW_ASSERT(
        !ZSTD_isError(remaining), "Zstd decompression failed: {}.",
        ZSTD_getErrorName(remaining));
    MW_ASSERT(
        destination.pos <= max_output_size - output.size(),
        "Zstd output exceeds the {} byte limit.", max_output_size);
    output.append(buffer.data(), destination.pos);
    MW_ASSERT(
        destination.pos != 0 || source.pos < source.size || remaining == 0,
        "Truncated Zstd frame.");
  }
  MW_ASSERT(source.pos == source.size, "Trailing data after Zstd frame.");
  return output;
}

std::string CompressLz4(std::string_view input, int compression_level) {
  LZ4F_preferences_t preferences{};
  preferences.frameInfo.contentSize = input.size();
  preferences.compressionLevel = compression_level;
  const size_t bound = LZ4F_compressFrameBound(input.size(), &preferences);
  MW_ASSERT(
      !LZ4F_isError(bound), "Cannot calculate LZ4 compression bound: {}.",
      LZ4F_getErrorName(bound));
  std::string output(bound, '\0');
  const size_t size = LZ4F_compressFrame(
      output.data(), output.size(), input.data(), input.size(), &preferences);
  MW_ASSERT(
      !LZ4F_isError(size), "LZ4 compression failed: {}.",
      LZ4F_getErrorName(size));
  output.resize(size);
  return output;
}

std::string DecompressLz4(std::string_view input, size_t max_output_size) {
  LZ4F_dctx* context = nullptr;
  const size_t created =
      LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
  MW_ASSERT(
      !LZ4F_isError(created) && context != nullptr,
      "Cannot create an LZ4 decompressor: {}.", LZ4F_getErrorName(created));
  struct Guard {
    LZ4F_dctx* context;
    ~Guard() { LZ4F_freeDecompressionContext(context); }
  } guard{context};

  size_t source_offset = 0;
  size_t remaining = 1;
  std::array<char, kStreamBufferSize> buffer{};
  std::string output;
  {
    // The declared size is only a reservation hint; the cap below still holds.
    LZ4F_frameInfo_t info{};
    size_t source_size = input.size();
    remaining = LZ4F_getFrameInfo(context, &info, input.data(), &source_size);
    MW_ASSERT(
        !LZ4F_isError(remaining), "LZ4 decompression failed: {}.",
        LZ4F_getErrorName(remaining));
    source_offset = source_size;
    output.reserve(DecompressReserveHint(info.contentSize, max_output_size));
  }
  while (source_offset < input.size() || remaining != 0) {
    size_t source_size = input.size() - source_offset;
    size_t destination_size = buffer.size();
    remaining = LZ4F_decompress(
        context, buffer.data(), &destination_size,
        input.data() + source_offset, &source_size, nullptr);
    MW_ASSERT(
        !LZ4F_isError(remaining), "LZ4 decompression failed: {}.",
        LZ4F_getErrorName(remaining));
    source_offset += source_size;
    MW_ASSERT(
        destination_size <= max_output_size - output.size(),
        "LZ4 output exceeds the {} byte limit.", max_output_size);
    output.append(buffer.data(), destination_size);
    MW_ASSERT(
        destination_size != 0 || source_size != 0 || remaining == 0,
        "Truncated LZ4 frame.");
  }
  MW_ASSERT(source_offset == input.size(), "Trailing data after LZ4 frame.");
  return output;
}

}  // namespace

std::string CompressBuffer(
    std::string_view input,
    std::string_view format,
    int compression_level) {
  if (format == "zstd") {
    return CompressZstd(input, compression_level);
  }
  if (format == "lz4") {
    return CompressLz4(input, compression_level);
  }
  MW_ASSERT(false, "Unsupported compression format '{}'.", format);
  return {};
}

std::string DecompressBuffer(
    std::string_view input,
    std::string_view format,
    size_t max_output_size) {
  if (format == "zstd") {
    return DecompressZstd(input, max_output_size);
  }
  if (format == "lz4") {
    return DecompressLz4(input, max_output_size);
  }
  MW_ASSERT(false, "Unsupported compression format '{}'.", format);
  return {};
}

std::filesystem::path CompressedFilePath(
    const std::filesystem::path& input_path,
    std::string_view format) {
  return input_path.string() + "." + std::string(format);
}

std::filesystem::path CompressFile(
    const std::filesystem::path& input_path,
    std::string_view format,
    int compression_level) {
  const auto output_path = CompressedFilePath(input_path, format);
  const std::string compressed = CompressBuffer(ReadFile(input_path), format, compression_level);
  // The input is only removed once the output and its name are on disk: a
  // power loss leaves one of them complete. This runs on the compression
  // pool, the fsync does not block recording.
  TemporaryFile temporary = CreateTemporaryFile(output_path);
  WriteAll(temporary, compressed);
  MW_ASSERT(::fsync(temporary.fd) == 0, "Cannot sync '{}': {}.", temporary.path, std::strerror(errno));
  MW_ASSERT(::close(std::exchange(temporary.fd, -1)) == 0, "Cannot close '{}': {}.", temporary.path,
            std::strerror(errno));
  std::filesystem::rename(temporary.path, output_path);
  temporary.path.clear();
  SyncDirectory(output_path.parent_path().empty() ? "." : output_path.parent_path());
  std::filesystem::remove(input_path);
  return output_path;
}

std::string ReadCompressedFile(
    const std::filesystem::path& input_path,
    std::string_view format,
    size_t max_output_size) {
  return DecompressBuffer(ReadFile(input_path), format, max_output_size);
}

}  // namespace ibmw::extensions::record_playback_extension
