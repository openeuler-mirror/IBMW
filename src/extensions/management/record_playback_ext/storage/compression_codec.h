// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ibmw::extensions::record_playback_extension {

std::string CompressBuffer(
    std::string_view input,
    std::string_view format,
    int compression_level = 0);

std::string DecompressBuffer(
    std::string_view input,
    std::string_view format);

std::filesystem::path CompressedFilePath(
    const std::filesystem::path& input_path,
    std::string_view format);

std::filesystem::path CompressFile(
    const std::filesystem::path& input_path,
    std::string_view format,
    int compression_level = 0);

std::string ReadCompressedFile(
    const std::filesystem::path& input_path,
    std::string_view format);

}  // namespace ibmw::extensions::record_playback_extension
