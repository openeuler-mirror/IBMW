#pragma once

#include "dds_ext/fastdds_compat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#if __has_include(<fastdds/rtps/common/SampleIdentity.hpp>)
#include <fastdds/rtps/common/SampleIdentity.hpp>
#else
#include <fastdds/rtps/common/SampleIdentity.h>
#endif
#if __has_include(<fastdds/rtps/common/WriteParams.hpp>)
#include <fastdds/rtps/common/WriteParams.hpp>
#else
#include <fastdds/rtps/common/WriteParams.h>
#endif

namespace ibmw::extensions::dds_extension::comm_trace {

inline constexpr uint32_t kAbiVersion = 1;
inline constexpr uint8_t kSchemaVersion = 1;
inline constexpr size_t kRingCapacity = 4096;
inline constexpr const char kExactCapabilityId[] =
    "dds.ordinary.nonplain.single_driver.sync_callback";

enum class RuntimeMode : uint8_t { off = 0, sampled = 1, full = 2 };
enum class EventKind : uint8_t {
  publish_write = 1,
  take_receive = 2,
  dispatch = 3,
};

// FNV-1a 32 is applied to the exact DDS topic-name bytes. topic_hash is only a
// label; it is never part of sample or delivery identity.
uint32_t TopicHash(const char* data, size_t size) noexcept;

struct alignas(8) Event final {
  uint64_t start_timestamp_ns;
  uint64_t end_timestamp_ns;
  uint8_t writer_guid[16];
  int32_t sequence_high;
  uint32_t sequence_low;
  uint8_t reader_guid[16];
  uint32_t topic_hash;
  uint8_t kind;
  uint8_t status;
  uint8_t schema_version;
  uint8_t reserved;
};
static_assert(sizeof(Event) == 64);
static_assert(alignof(Event) == 8);
static_assert(offsetof(Event, start_timestamp_ns) == 0);
static_assert(offsetof(Event, end_timestamp_ns) == 8);
static_assert(offsetof(Event, writer_guid) == 16);
static_assert(offsetof(Event, sequence_high) == 32);
static_assert(offsetof(Event, sequence_low) == 36);
static_assert(offsetof(Event, reader_guid) == 40);
static_assert(offsetof(Event, topic_hash) == 56);
static_assert(offsetof(Event, kind) == 60);
static_assert(offsetof(Event, status) == 61);
static_assert(offsetof(Event, schema_version) == 62);
static_assert(offsetof(Event, reserved) == 63);
static_assert(std::is_standard_layout_v<Event>);
static_assert(std::is_trivial_v<Event>);

// SampleKey is exactly writer GUID + writer sequence number. It is stable from
// publish through DDS receive and does not include topic_hash.
struct SampleKey final {
  uint8_t writer_guid[16]{};
  int32_t sequence_high{-1};
  uint32_t sequence_low{0};
};

// DeliveryKey is SampleKey + the DataReader GUID that accepted the sample.
// It identifies a DDS reader delivery, not a callback registration.
struct DeliveryKey final {
  SampleKey sample{};
  uint8_t reader_guid[16]{};
};

enum class ExactEventKind : uint8_t { callback_span = 1 };

// Independent exact-callback record. It does not extend or reinterpret the
// stable 64-byte stage-only Event format.
struct alignas(8) ExactEvent final {
  uint64_t start_timestamp_ns;
  uint64_t end_timestamp_ns;
  uint8_t writer_guid[16];
  int32_t sequence_high;
  uint32_t sequence_low;
  uint8_t reader_guid[16];
  uint64_t callback_registration_id;
  uint8_t kind;
  uint8_t status;
  uint8_t schema_version;
  uint8_t reserved[5];
};
static_assert(sizeof(ExactEvent) == 72);
static_assert(alignof(ExactEvent) == 8);
static_assert(offsetof(ExactEvent, callback_registration_id) == 56);
static_assert(offsetof(ExactEvent, kind) == 64);
static_assert(std::is_standard_layout_v<ExactEvent>);
static_assert(std::is_trivial_v<ExactEvent>);

struct ExactSnapshot final {
  uint32_t schema_version;
  uint32_t record_size;
  uint64_t pending;
  uint64_t dropped;
  uint64_t invalid_identity;
  uint64_t missing_registration;
  uint64_t unsupported_async;
  uint64_t unsupported_multi_handler;
};
static_assert(sizeof(ExactSnapshot) == 56);

struct ExactQuery final {
  uint32_t schema_version;
  uint32_t struct_size;
  size_t (*drain)(ExactEvent*, size_t) noexcept;
  ExactSnapshot (*snapshot)() noexcept;
};
static_assert(sizeof(ExactQuery) == 24);

using Guid = fastdds_compat::rtps::GUID_t;
using SampleIdentity = fastdds_compat::rtps::SampleIdentity;
using WriteParams = fastdds_compat::rtps::WriteParams;

void CopyGuid(const Guid& source, uint8_t destination[16]) noexcept;
bool GuidBytesKnown(const uint8_t guid[16]) noexcept;
bool SampleKeyValid(const SampleKey& key) noexcept;
bool MakeSampleKey(const SampleIdentity& identity, SampleKey* output) noexcept;
bool MakeDeliveryKey(const SampleIdentity& identity, const Guid& reader_guid,
                     DeliveryKey* output) noexcept;
bool WriterMatches(const SampleKey& key, const Guid& writer_guid) noexcept;
bool ShouldSample(const SampleKey& key) noexcept;

struct SnapshotV1 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t dropped;
  uint64_t invalid_identity;
  uint64_t unsupported_async;
  uint64_t pending;
  // Trace-finalization evidence only. active_producers counts producers inside
  // the trace enqueue path, and sealed rejects new trace records. Neither field
  // proves that calling threads have exited the extension DSO, and they must
  // never be used to authorize dlclose.
  uint32_t active_producers;
  uint8_t sealed;
  uint8_t reserved[3];
};
static_assert(sizeof(SnapshotV1) == 48);
static_assert(alignof(SnapshotV1) == 8);
static_assert(offsetof(SnapshotV1, abi_version) == 0);
static_assert(offsetof(SnapshotV1, struct_size) == 4);
static_assert(offsetof(SnapshotV1, dropped) == 8);
static_assert(offsetof(SnapshotV1, invalid_identity) == 16);
static_assert(offsetof(SnapshotV1, unsupported_async) == 24);
static_assert(offsetof(SnapshotV1, pending) == 32);
static_assert(offsetof(SnapshotV1, active_producers) == 40);
static_assert(offsetof(SnapshotV1, sealed) == 44);
static_assert(std::is_standard_layout_v<SnapshotV1>);
static_assert(std::is_trivial_v<SnapshotV1>);

struct QueryV1 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t reserved;
  void (*initialize)() noexcept;
  size_t (*drain)(Event*, size_t) noexcept;
  SnapshotV1 (*snapshot)() noexcept;
  void (*seal)() noexcept;
};
static_assert(sizeof(QueryV1) == 48);
static_assert(alignof(QueryV1) == 8);
static_assert(offsetof(QueryV1, abi_version) == 0);
static_assert(offsetof(QueryV1, struct_size) == 4);
static_assert(offsetof(QueryV1, reserved) == 8);
static_assert(offsetof(QueryV1, initialize) == 16);
static_assert(offsetof(QueryV1, drain) == 24);
static_assert(offsetof(QueryV1, snapshot) == 32);
static_assert(offsetof(QueryV1, seal) == 40);
static_assert(std::is_standard_layout_v<QueryV1>);
static_assert(std::is_trivial_v<QueryV1>);

inline constexpr uint32_t kAbiVersionV2 = 2;

struct SnapshotV2 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t dropped;
  uint64_t invalid_identity;
  uint64_t unsupported_async;
  uint64_t pending;
  uint64_t eligible;
  uint64_t sampled_out;
  uint64_t accepted;
  uint64_t exported;
  uint64_t export_discarded;
  uint32_t active_producers;
  uint32_t owners;
  uint8_t sealed;
  uint8_t managed;
  uint8_t io_error;
  uint8_t reserved;
};
static_assert(sizeof(SnapshotV2) == 96);
static_assert(std::is_standard_layout_v<SnapshotV2>);
static_assert(std::is_trivial_v<SnapshotV2>);

struct QueryV2 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t reserved;
  SnapshotV2 (*snapshot)() noexcept;
  void (*request_seal)() noexcept;
};
static_assert(sizeof(QueryV2) == 32);
static_assert(std::is_standard_layout_v<QueryV2>);
static_assert(std::is_trivial_v<QueryV2>);

// Trace evidence finalization contract:
// 1. Extension shutdown first quiesces every DDS callback that can submit trace
//    records, then calls seal().
// 2. The consumer waits until snapshot() reports sealed != 0 and
//    active_producers == 0, then one consumer calls drain() until it returns 0.
// 3. After that drain() call has returned, the consumer calls snapshot() and
//    finalizes the evidence only when pending == 0, recording dropped and the
//    other counters as part of the final result. snapshot() and drain() must
//    themselves return before evidence is finalized.
// pending is approximate while producers run. Once sealed is observed and
// active_producers is zero, it is stable except for the single consumer's drain.
// This query does not provide general dynamic-unload safety. The normal
// ExtensionLoader intentionally leaks its handle; no result here permits
// dlclose.

using ClockForTest = uint64_t (*)() noexcept;
using ProducerGateHookForTest = void (*)() noexcept;

// Returns false only when tracing was requested but its frozen runtime binding
// could not be validated or initialized. Runtime-off is a successful no-op.
__attribute__((visibility("default"))) bool Initialize() noexcept;
__attribute__((visibility("default"))) void Shutdown() noexcept;
RuntimeMode mode() noexcept;
bool active() noexcept;
uint64_t NowNs() noexcept;
void Submit(Event event) noexcept;
void InvalidIdentity() noexcept;
void UnsupportedAsync() noexcept;
Event MakeEvent(uint64_t start_ns, uint64_t end_ns, const SampleKey& key,
                uint32_t topic_hash, EventKind kind, uint8_t status = 0) noexcept;
Event MakeEvent(uint64_t start_ns, uint64_t end_ns, const DeliveryKey& key,
                uint32_t topic_hash, EventKind kind, uint8_t status = 0) noexcept;

bool ExactActive() noexcept;
inline bool ShouldEmitPublishWrite(bool valid_publish_envelope) noexcept {
  return valid_publish_envelope || !ExactActive();
}
bool ExactCallbackBegin() noexcept;
void ExactCallbackEnd() noexcept;
void ExactSubmitCallback(uint64_t start_ns, uint64_t end_ns,
                         const DeliveryKey& key,
                         uint64_t callback_registration_id,
                         uint8_t status = 0) noexcept;
void ExactInvalidIdentity() noexcept;
void ExactMissingRegistration() noexcept;
void ExactUnsupportedAsync() noexcept;
void ExactUnsupportedMultiHandler() noexcept;
const ExactQuery* ExactQueryInternal() noexcept;

#ifdef IBMW_COMM_TRACE_TEST_HOOKS
__attribute__((visibility("hidden"))) void InjectStageDroppedForTest() noexcept;
__attribute__((visibility("hidden"))) void InjectExactDroppedForTest() noexcept;

struct RuntimeFaultHooksForTest final {
  uint32_t abi_version;
  uint32_t struct_size;
  void (*inject_stage_dropped)() noexcept;
  void (*inject_exact_dropped)() noexcept;
  uint64_t (*now_ns)() noexcept;
  uint32_t (*topic_hash)(const char*, size_t) noexcept;
  Event (*make_stage_event)(uint64_t, uint64_t, const SampleKey&, uint32_t,
                            EventKind, uint8_t) noexcept;
  void (*submit)(Event) noexcept;
  void (*exact_submit)(uint64_t, uint64_t, const DeliveryKey&, uint64_t,
                       uint8_t) noexcept;
};
#endif

void ResetForTest() noexcept;
void SetModeForTest(RuntimeMode value) noexcept;
void SetClockForTest(ClockForTest clock) noexcept;
void SetWriterFailureForTest(bool value) noexcept;
void SetDirectorySyncFailureForTest(bool value) noexcept;
void SetProducerGateHookForTest(ProducerGateHookForTest hook) noexcept;
void SetExactEnabledForTest(bool value) noexcept;

}  // namespace ibmw::extensions::dds_extension::comm_trace

extern "C" __attribute__((visibility("default")))
const ibmw::extensions::dds_extension::comm_trace::QueryV1*
ibmw_dds_comm_trace_query_v1() noexcept;

extern "C" __attribute__((visibility("default")))
const ibmw::extensions::dds_extension::comm_trace::QueryV2*
ibmw_dds_comm_trace_query_v2() noexcept;
