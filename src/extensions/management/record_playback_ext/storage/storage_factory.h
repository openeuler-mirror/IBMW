// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "record_playback_ext/storage/storage.h"

namespace ibmw::extensions::record_playback_extension {

using StoragePluginFactory =
    std::function<std::unique_ptr<Storage>()>;

__attribute__((visibility("default"))) void RegisterStoragePlugin(
    std::string storage_id,
    StoragePluginFactory factory);

__attribute__((visibility("default"))) std::unique_ptr<Storage> CreateStorage(
    std::string_view storage_id);

__attribute__((visibility("default"))) std::vector<std::string>
AvailableStoragePlugins();

}  // namespace ibmw::extensions::record_playback_extension
