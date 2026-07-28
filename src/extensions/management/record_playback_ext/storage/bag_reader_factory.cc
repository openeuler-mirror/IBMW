// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/bag_reader_factory.h"

#include <map>
#include <mutex>

#include "base/exception.h"
#include "record_playback_ext/storage/mcap_bag_reader.h"
#include "record_playback_ext/storage/sqlite_bag_reader.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

struct BagReaderPluginRegistry {
  std::mutex mutex;
  std::map<std::string, BagReaderPluginFactory> factories;
};

BagReaderPluginRegistry& Registry() {
  static BagReaderPluginRegistry registry;
  return registry;
}

void EnsureBuiltinPlugins() {
  static const bool registered = [] {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    registry.factories.emplace(
        "mcap", [] { return std::make_unique<McapBagReader>(); });
    registry.factories.emplace(
        "sqlite3", [] { return std::make_unique<SqliteBagReader>(); });
    return true;
  }();
  static_cast<void>(registered);
}

std::string InferFileStorageId(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  if (path.extension() == ".db3") {
    return "sqlite3";
  }
  if (path.extension() == ".mcap" || filename.ends_with(".mcap.zstd") ||
      filename.ends_with(".mcap.lz4")) {
    return "mcap";
  }
  return {};
}

}  // namespace

void RegisterBagReaderPlugin(
    std::string storage_id,
    BagReaderPluginFactory factory) {
  EnsureBuiltinPlugins();
  MW_ASSERT(!storage_id.empty(), "Bag Reader Plugin ID cannot be empty.");
  MW_ASSERT(factory, "Bag Reader Plugin '{}' has no factory.", storage_id);
  auto& registry = Registry();
  std::lock_guard lock(registry.mutex);
  MW_ASSERT(
      registry.factories.emplace(storage_id, std::move(factory)).second,
      "Bag Reader Plugin '{}' is already registered.", storage_id);
}

std::unique_ptr<BagReader> CreateBagReader(
    const std::filesystem::path& bag_path,
    const ReaderOpenOptions& options) {
  MW_ASSERT(
      options.storage_config_file.empty() ||
          std::filesystem::is_regular_file(options.storage_config_file),
      "Reader storage config '{}' does not exist.",
      options.storage_config_file.string());
  const auto absolute = std::filesystem::absolute(bag_path);
  std::string storage_id;
  if (std::filesystem::is_directory(absolute)) {
    const auto metadata_path = absolute / "metadata.yaml";
    MW_ASSERT(
        std::filesystem::is_regular_file(metadata_path),
        "Bag metadata '{}' does not exist.", metadata_path.string());
    const auto root = YAML::LoadFile(metadata_path.string());
    storage_id =
        root["ibmw_bagfile_information"]["storage_id"].as<std::string>();
  } else {
    MW_ASSERT(
        std::filesystem::is_regular_file(absolute),
        "Bag file '{}' does not exist.", absolute.string());
    storage_id = InferFileStorageId(absolute);
    MW_ASSERT(
        !storage_id.empty(), "Cannot infer Storage Plugin for bag file '{}'.",
        absolute.string());
  }
  MW_ASSERT(
      options.expected_storage_id.empty() ||
          options.expected_storage_id == storage_id,
      "Requested storage '{}' does not match bag storage '{}'.",
      options.expected_storage_id, storage_id);
  EnsureBuiltinPlugins();
  BagReaderPluginFactory factory;
  {
    auto& registry = Registry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.factories.find(storage_id);
    MW_ASSERT(
        found != registry.factories.end(),
        "No Reader Plugin for storage '{}'.", storage_id);
    factory = found->second;
  }
  auto reader = factory();
  MW_ASSERT(
      reader != nullptr,
      "Bag Reader Plugin '{}' returned a null reader.", storage_id);
  reader->Open(bag_path, options);
  return reader;
}

std::vector<std::string> AvailableBagReaderPlugins() {
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
