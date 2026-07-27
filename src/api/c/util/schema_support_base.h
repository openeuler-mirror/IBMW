// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 UINT32_C(1)
#define IBMW_SCHEMA_SUPPORT_V1_RESERVED_COUNT 8u

/*
 * The first two fields are present in every revision of the descriptor.  A
 * consumer must read struct_size before accessing any field after that
 * prefix.  V1 producers set struct_size to sizeof(their descriptor); V1
 * consumers accept larger values so that a later revision can append fields.
 */
#define IBMW_SCHEMA_SUPPORT_V1_PREFIX_SIZE \
  (2u * sizeof(uint32_t))

/**
 * @brief Stable C ABI describing one message schema.
 *
 * All string views and the structure itself are owned by the exporting type
 * support package and remain valid until that package is unloaded. Names,
 * encodings, schema bytes and hashes are immutable for that lifetime. Empty
 * schemas are invalid. type_name is the canonical runtime name, including
 * its family prefix (`dds:`, `ros2:`, or `json:`). serialization_type and
 * schema_encoding use the framework's canonical lower-case identifiers (for
 * example `cdr`, `ros2`, `json`, `omgidl`, `ros2msg`, and `jsonschema`). V1
 * hashes are FNV-1a over exactly schema_data.len bytes and use
 * `fnv1a64:<16 lowercase hex digits>`. A consumer must compare the full
 * descriptor and schema bytes rather than treating the 64-bit hash as a
 * collision-free identity.
 */
typedef struct ibmw_schema_support_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw_string_view_t type_name;
  ibmw_string_view_t serialization_type;
  ibmw_string_view_t schema_name;
  ibmw_string_view_t schema_encoding;
  ibmw_string_view_t schema_data;
  ibmw_string_view_t schema_hash;
  /* Reserved for append-only V1 extensions; producers initialize to zero. */
  uint64_t reserved[IBMW_SCHEMA_SUPPORT_V1_RESERVED_COUNT];
} ibmw_schema_support_v1_t;

/* Number of bytes required for all mandatory V1 schema fields (excluding the
 * reserved/extension tail).  This is intentionally an offsetof expression so
 * consumers can validate truncated descriptors without dereferencing fields
 * that are not covered by struct_size.
 */
#define IBMW_SCHEMA_SUPPORT_V1_REQUIRED_SIZE \
  offsetof(ibmw_schema_support_v1_t, reserved)

/**
 * @brief V1 package-level SchemaSupport capability returned by a TypeSupport
 * package.
 */
typedef struct ibmw_schema_support_package_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  size_t entry_count;
  const ibmw_schema_support_v1_t* const* entries;
  /* Reserved for append-only V1 extensions; producers initialize to zero. */
  uint64_t reserved[IBMW_SCHEMA_SUPPORT_V1_RESERVED_COUNT];
} ibmw_schema_support_package_v1_t;

#define IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE \
  offsetof(ibmw_schema_support_package_v1_t, reserved)

typedef ibmw_schema_support_v1_t ibmw_schema_support_base_t;

#ifdef __cplusplus
}
#endif
