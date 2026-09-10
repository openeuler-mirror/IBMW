// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/schema/schema_registry.h"

#include <functional>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

size_t SchemaRegistry::KeyHash::operator()(const Key& key) const noexcept {
  const size_t type_hash = std::hash<std::string>{}(key.type_name);
  const size_t serialization_hash =
      std::hash<std::string>{}(key.serialization_type);
  return type_hash ^ (serialization_hash + 0x9e3779b9 + (type_hash << 6) +
                      (type_hash >> 2));
}

SchemaRegistry::SchemaId SchemaRegistry::Register(
    ibmw::util::SchemaSupportRef schema_support) {
  MW_ASSERT(schema_support, "Cannot register a null Schema Support.");
  return Register(schema_support.ToDescriptor());
}

SchemaRegistry::SchemaId SchemaRegistry::Register(
    ibmw::util::SchemaDescriptor descriptor) {
  MW_ASSERT(!descriptor.type_name.empty(), "Schema type name cannot be empty.");
  MW_ASSERT(
      !descriptor.serialization_type.empty(),
      "Schema serialization type cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_name.empty(), "Schema name cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_encoding.empty(),
      "Schema encoding cannot be empty for '{}'.", descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_data.empty(), "Schema data cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_hash.empty(), "Schema hash cannot be empty for '{}'.",
      descriptor.type_name);

  const std::string expected_hash =
      ibmw::util::ComputeSchemaHash(descriptor.schema_data);
  MW_ASSERT(
      descriptor.schema_hash == expected_hash,
      "Schema hash mismatch for '{}': expected '{}', got '{}'.",
      descriptor.type_name, expected_hash, descriptor.schema_hash);

  Key key{
      .type_name = descriptor.type_name,
      .serialization_type = descriptor.serialization_type};
  auto existing = schemas_by_key_.find(key);
  if (existing != schemas_by_key_.end()) {
    const auto& old = existing->second.descriptor;
    MW_ASSERT(
        old.schema_hash == descriptor.schema_hash &&
            old.schema_name == descriptor.schema_name &&
            old.schema_encoding == descriptor.schema_encoding &&
            old.schema_data == descriptor.schema_data,
        "Conflicting Schema Support for type '{}' and serialization '{}'.",
        descriptor.type_name, descriptor.serialization_type);
    return existing->second.id;
  }

  const SchemaId id = next_id_++;
  schemas_by_key_.emplace(
      std::move(key), Entry{.id = id, .descriptor = std::move(descriptor)});
  return id;
}

std::optional<ibmw::util::SchemaDescriptor> SchemaRegistry::Find(
    std::string_view type_name,
    std::string_view serialization_type) const {
  const auto found = schemas_by_key_.find(
      Key{.type_name = std::string(type_name),
          .serialization_type = std::string(serialization_type)});
  if (found == schemas_by_key_.end()) {
    return std::nullopt;
  }
  return found->second.descriptor;
}

}  // namespace ibmw::extensions::record_playback_extension
