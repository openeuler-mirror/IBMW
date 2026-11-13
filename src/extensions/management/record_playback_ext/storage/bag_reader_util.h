// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Helpers shared by the bag readers and the runtime playback stream (EXT-044).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "record_playback_ext/storage/bag_reader.h"
#include "cpp/util/buffer.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::record_playback_extension {

// Returns a view that keeps `data` alive. The container and the view share
// one allocation; the view is never moved, so its pointers stay valid.
template <typename Container>
std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(Container data) {
  struct Owned {
    explicit Owned(Container&& bytes) : data(std::move(bytes)), view(data.data(), data.size()) {}
    Container data;
    ibmw::util::BufferArrayView view;
  };
  auto owned = std::make_shared<Owned>(std::move(data));
  return std::shared_ptr<ibmw::util::BufferArrayView>(owned, &owned->view);
}

inline std::shared_ptr<ibmw::util::BufferArrayView> MakeOwnedPayload(const void* data, size_t size) {
  std::vector<uint8_t> bytes(size);
  if (size != 0) std::memcpy(bytes.data(), data, size);
  return MakeOwnedPayload(std::move(bytes));
}

// A file entry in metadata.yaml matches the file when every field it records
// equals the file's own summary; missing fields are not compared.
inline bool MetadataFileMatches(const YAML::Node& metadata, const BagFileInfo& actual) {
  return !metadata || !metadata.IsMap() ||
         (metadata["size"].as<uint64_t>(actual.size) == actual.size &&
          metadata["message_count"].as<uint64_t>(actual.message_count) == actual.message_count &&
          metadata["start_timestamp"].as<uint64_t>(actual.start_timestamp) == actual.start_timestamp &&
          metadata["end_timestamp"].as<uint64_t>(actual.end_timestamp) == actual.end_timestamp);
}

}  // namespace ibmw::extensions::record_playback_extension
