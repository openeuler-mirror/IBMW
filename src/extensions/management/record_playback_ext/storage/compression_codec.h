// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ibmw::extensions::record_playback_extension {

// Upper bounds on decompressed output; a stream exceeding them is rejected
// instead of growing until OOM (decompression bomb / corrupted frame).
inline constexpr size_t kMaxDecompressedPayloadSize = size_t{1} << 30;  // 1 GiB
inline constexpr size_t kMaxDecompressedFileSize = size_t{4} << 30;     // 4 GiB

std::string CompressBuffer(
    std::string_view input,
    std::string_view format,
    int compression_level = 0);

std::string DecompressBuffer(
    std::string_view input,
    std::string_view format,
    size_t max_output_size = kMaxDecompressedPayloadSize);

std::filesystem::path CompressedFilePath(
    const std::filesystem::path& input_path,
    std::string_view format);

std::filesystem::path CompressFile(
    const std::filesystem::path& input_path,
    std::string_view format,
    int compression_level = 0);

std::string ReadCompressedFile(
    const std::filesystem::path& input_path,
    std::string_view format,
    size_t max_output_size = kMaxDecompressedFileSize);

}  // namespace ibmw::extensions::record_playback_extension
