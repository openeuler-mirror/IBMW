// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG.
#ifndef IBMW_REPORTER_V1_H_
#define IBMW_REPORTER_V1_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define IBMW_REPORTER_ABI_VERSION_V1 UINT32_C(1)
#define IBMW_REPORTER_V1_RESERVED_COUNT 8u

#define IBMW_REPORTER_EXPORT __attribute__((visibility("default")))

typedef struct ibmw_reporter_string_view_v1 {
  const char* data;
  uint64_t size;
} ibmw_reporter_string_view_v1;

typedef struct ibmw_reporter_slot_v1 ibmw_reporter_slot_v1;

typedef struct ibmw_reporter_slot_descriptor_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw_reporter_string_view_v1 kind;
  ibmw_reporter_string_view_v1 metric;
  ibmw_reporter_string_view_v1 topic;
  ibmw_reporter_string_view_v1 type;
  ibmw_reporter_string_view_v1 driver;
  uint64_t reserved[IBMW_REPORTER_V1_RESERVED_COUNT];
} ibmw_reporter_slot_descriptor_v1;

typedef struct ibmw_reporter_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* owner;
  ibmw_reporter_slot_v1* (*register_slot)(
      void* owner, const ibmw_reporter_slot_descriptor_v1* descriptor);
  void (*add)(void* owner, ibmw_reporter_slot_v1* slot, uint64_t value);
  uint64_t reserved[IBMW_REPORTER_V1_RESERVED_COUNT];
  // Optional trailing V1 extension. Consumers must check struct_size before
  // reading this member. Implementations clamp negative deltas at zero.
  void (*add_signed)(void* owner, ibmw_reporter_slot_v1* slot, int64_t delta);
} ibmw_reporter_v1;

// Optional companion API. This is deliberately separate from ibmw_reporter_v1
// so the V1 layout remains frozen. A transaction is owned by the API owner and
// must be completed exactly once by the matching owner.
#define IBMW_REPORTER_BATCH_ABI_VERSION_V1 UINT32_C(1)
typedef struct ibmw_reporter_batch_transaction_v1
    ibmw_reporter_batch_transaction_v1;
typedef struct ibmw_reporter_batch_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  void* owner;
  int (*prepare)(void* owner,
                 const ibmw_reporter_slot_descriptor_v1* descriptors,
                 size_t count, ibmw_reporter_slot_v1** slots,
                 ibmw_reporter_batch_transaction_v1** transaction);
  int (*commit)(void* owner,
                ibmw_reporter_batch_transaction_v1* transaction);
  void (*abort)(void* owner,
                ibmw_reporter_batch_transaction_v1* transaction);
} ibmw_reporter_batch_v1;

IBMW_REPORTER_EXPORT const ibmw_reporter_batch_v1*
ibmw_reporter_query_batch_v1(void);

// Returns NULL when reporter support is compiled out or disabled at runtime.
IBMW_REPORTER_EXPORT const ibmw_reporter_v1* ibmw_reporter_query_v1(void);

#if defined(__cplusplus)
}  // extern "C"
#endif
#endif  // IBMW_REPORTER_V1_H_
