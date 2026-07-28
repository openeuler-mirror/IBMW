// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "record_playback_ext/storage/mcap_bag_reader.h"

namespace ibmw::extensions::record_playback_extension {

using BagReaderPluginFactory =
    std::function<std::unique_ptr<BagReader>()>;

__attribute__((visibility("default"))) void RegisterBagReaderPlugin(
    std::string storage_id,
    BagReaderPluginFactory factory);

__attribute__((visibility("default"))) std::unique_ptr<BagReader>
CreateBagReader(
    const std::filesystem::path& bag_path,
    const ReaderOpenOptions& options = {});

__attribute__((visibility("default"))) std::vector<std::string>
AvailableBagReaderPlugins();

}  // namespace ibmw::extensions::record_playback_extension
