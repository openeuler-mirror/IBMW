// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "c/util/schema_support_base.h"
#include "cpp/util/string.h"

namespace ibmw::util {

struct SchemaDescriptor {
  std::string type_name;
  std::string serialization_type;
  std::string schema_name;
  std::string schema_encoding;
  std::string schema_data;
  std::string schema_hash;
};

/**
 * @brief Return the cross-platform hash format used by Schema Support v1.
 */
inline std::string ComputeSchemaHash(std::string_view schema_data) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

  uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char byte : schema_data) {
    hash ^= byte;
    hash *= kFnvPrime;
  }

  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
         << hash;
  return output.str();
}

class SchemaSupportRef {
 public:
  SchemaSupportRef() = default;
  explicit SchemaSupportRef(const ibmw_schema_support_base_t* base_ptr)
      : base_ptr_(base_ptr) {}

  explicit operator bool() const { return base_ptr_ != nullptr; }

  const ibmw_schema_support_base_t* NativeHandle() const { return base_ptr_; }

  std::string_view TypeName() const {
    return ToStdStringView(base_ptr_->type_name);
  }

  std::string_view SerializationType() const {
    return ToStdStringView(base_ptr_->serialization_type);
  }

  std::string_view SchemaName() const {
    return ToStdStringView(base_ptr_->schema_name);
  }

  std::string_view SchemaEncoding() const {
    return ToStdStringView(base_ptr_->schema_encoding);
  }

  std::string_view SchemaData() const {
    return ToStdStringView(base_ptr_->schema_data);
  }

  std::string_view SchemaHash() const {
    return ToStdStringView(base_ptr_->schema_hash);
  }

  SchemaDescriptor ToDescriptor() const {
    return SchemaDescriptor{
        .type_name = std::string(TypeName()),
        .serialization_type = std::string(SerializationType()),
        .schema_name = std::string(SchemaName()),
        .schema_encoding = std::string(SchemaEncoding()),
        .schema_data = std::string(SchemaData()),
        .schema_hash = std::string(SchemaHash())};
  }

 private:
  const ibmw_schema_support_base_t* base_ptr_ = nullptr;
};

/**
 * @brief Owns strings backing an ibmw_schema_support_base_t C ABI object.
 */
class SchemaSupport {
 public:
  explicit SchemaSupport(SchemaDescriptor descriptor)
      : descriptor_(std::move(descriptor)) {
    if (descriptor_.schema_hash.empty() && !descriptor_.schema_data.empty()) {
      descriptor_.schema_hash = ComputeSchemaHash(descriptor_.schema_data);
    }
    RefreshNativeHandle();
  }

  SchemaSupport(const SchemaSupport&) = delete;
  SchemaSupport& operator=(const SchemaSupport&) = delete;
  SchemaSupport(SchemaSupport&&) = delete;
  SchemaSupport& operator=(SchemaSupport&&) = delete;

  const ibmw_schema_support_base_t* NativeHandle() const { return &base_; }
  SchemaSupportRef Ref() const { return SchemaSupportRef(&base_); }

 private:
  void RefreshNativeHandle() {
    base_ = ibmw_schema_support_base_t{
        .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
        .struct_size = sizeof(ibmw_schema_support_base_t),
        .type_name = ToIbmwStringView(descriptor_.type_name),
        .serialization_type = ToIbmwStringView(descriptor_.serialization_type),
        .schema_name = ToIbmwStringView(descriptor_.schema_name),
        .schema_encoding = ToIbmwStringView(descriptor_.schema_encoding),
        .schema_data = ToIbmwStringView(descriptor_.schema_data),
        .schema_hash = ToIbmwStringView(descriptor_.schema_hash),
        .reserved = {}};
  }

  SchemaDescriptor descriptor_;
  ibmw_schema_support_base_t base_{};
};

}  // namespace ibmw::util
