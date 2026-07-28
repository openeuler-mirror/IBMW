// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/storage_factory.h"

#include <map>
#include <mutex>

#include "base/exception.h"
#include "record_playback_ext/storage/mcap_storage.h"
#include "record_playback_ext/storage/sqlite_storage.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

struct StoragePluginRegistry {
  std::mutex mutex;
  std::map<std::string, StoragePluginFactory> factories;
};

StoragePluginRegistry& Registry() {
  static StoragePluginRegistry registry;
  return registry;
}

void EnsureBuiltinPlugins() {
  static const bool registered = [] {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    registry.factories.emplace(
        "mcap", [] { return std::make_unique<McapStorage>(); });
    registry.factories.emplace(
        "sqlite3", [] { return std::make_unique<SqliteStorage>(); });
    return true;
  }();
  static_cast<void>(registered);
}

}  // namespace

void RegisterStoragePlugin(
    std::string storage_id,
    StoragePluginFactory factory) {
  EnsureBuiltinPlugins();
  MW_ASSERT(!storage_id.empty(), "Storage Plugin ID cannot be empty.");
  MW_ASSERT(factory, "Storage Plugin '{}' has no factory.", storage_id);
  auto& registry = Registry();
  std::lock_guard lock(registry.mutex);
  MW_ASSERT(
      registry.factories.emplace(storage_id, std::move(factory)).second,
      "Storage Plugin '{}' is already registered.", storage_id);
}

std::unique_ptr<Storage> CreateStorage(std::string_view storage_id) {
  EnsureBuiltinPlugins();
  auto& registry = Registry();
  StoragePluginFactory factory;
  {
    std::lock_guard lock(registry.mutex);
    const auto found = registry.factories.find(std::string(storage_id));
    MW_ASSERT(
        found != registry.factories.end(),
        "Unknown Storage Plugin '{}'.", storage_id);
    factory = found->second;
  }
  auto storage = factory();
  MW_ASSERT(
      storage != nullptr,
      "Storage Plugin '{}' returned a null writer.", storage_id);
  return storage;
}

std::vector<std::string> AvailableStoragePlugins() {
  EnsureBuiltinPlugins();
  auto& registry = Registry();
  std::lock_guard lock(registry.mutex);
  std::vector<std::string> result;
  result.reserve(registry.factories.size());
  for (const auto& [storage_id, factory] : registry.factories) {
    static_cast<void>(factory);
    result.emplace_back(storage_id);
  }
  return result;
}

}  // namespace ibmw::extensions::record_playback_extension
