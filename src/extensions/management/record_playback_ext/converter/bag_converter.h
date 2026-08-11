// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

struct BagConvertOptions {
  std::filesystem::path input_path;
  std::string input_storage_id;
  std::filesystem::path input_storage_config_file;
  StorageOptions output;
};

struct BagConvertResult {
  std::string input_storage_id;
  std::string output_storage_id;
  std::filesystem::path output_path;
  uint64_t input_message_count = 0;
  uint64_t output_message_count = 0;
  size_t schema_count = 0;
  size_t channel_count = 0;
  bool input_closed_cleanly = false;
};

BagConvertResult ConvertBag(const BagConvertOptions& options);

}  // namespace ibmw::extensions::record_playback_extension
