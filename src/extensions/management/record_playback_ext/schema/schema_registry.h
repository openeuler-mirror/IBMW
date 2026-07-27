// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cpp/util/schema_support.h"

namespace ibmw::extensions::record_playback_extension {

class SchemaRegistry {
 public:
  using SchemaId = uint64_t;

  SchemaId Register(ibmw::util::SchemaSupportRef schema_support);
  SchemaId Register(ibmw::util::SchemaDescriptor descriptor);

  std::optional<ibmw::util::SchemaDescriptor> Find(
      std::string_view type_name,
      std::string_view serialization_type) const;

  size_t Size() const { return schemas_by_key_.size(); }

 private:
  struct Key {
    std::string type_name;
    std::string serialization_type;

    bool operator==(const Key&) const = default;
  };

  struct KeyHash {
    size_t operator()(const Key& key) const noexcept;
  };

  struct Entry {
    SchemaId id;
    ibmw::util::SchemaDescriptor descriptor;
  };

  std::unordered_map<Key, Entry, KeyHash> schemas_by_key_;
  SchemaId next_id_ = 1;
};

}  // namespace ibmw::extensions::record_playback_extension
