// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ibmw::runtime::core::transport::internal {
struct PublishEnvelopeV1;
}

namespace ibmw::runtime::core::observability::comm_trace {

// Internal P0a synchronous wrapper-stage boundaries only. This is not exact
// asynchronous or cross-process correlation and does not infer DDS identity.
inline constexpr bool kStageOnly = true;
inline constexpr size_t kCapacity = 256;
inline constexpr size_t kMaxThreadRings = 64;

enum class EventKind : uint8_t {
  publish_enter = 0,
  publish_return = 1,
  callback_enter = 2,
  callback_exit = 3
};
enum class RuntimeMode : uint8_t { off = 0, sampled = 1, full = 2 };

struct Event final {
  uint64_t timestamp_ns;
  uint64_t token;
  uint32_t topic_hash;
  uint16_t driver_hash;
  EventKind kind;
  uint8_t reserved[9];
};
static_assert(sizeof(Event) == 32, "comm trace events must remain fixed width");
static_assert(alignof(Event) == alignof(uint64_t));
static_assert(offsetof(Event, timestamp_ns) == 0);
static_assert(offsetof(Event, token) == 8);
static_assert(offsetof(Event, topic_hash) == 16);
static_assert(offsetof(Event, driver_hash) == 20);
static_assert(offsetof(Event, kind) == 22);
static_assert(offsetof(Event, reserved) == 23);
static_assert(std::is_standard_layout_v<Event>);
static_assert(std::is_trivial_v<Event>);

#ifdef IBMW_ENABLE_COMM_TRACE
using ClockForTest = uint64_t (*)() noexcept;

void InitializeFromEnvironment() noexcept;
RuntimeMode mode() noexcept;
void SetModeForTest(RuntimeMode value) noexcept;
void SetClockForTest(ClockForTest clock) noexcept;
transport::internal::PublishEnvelopeV1 CapturePublishEnvelope() noexcept;
void Emit(EventKind kind, uint64_t token = 0, uint32_t topic_hash = 0,
          uint16_t driver_hash = 0) noexcept;
// Spans are consumed as enter/exit pairs. Capacity below two returns zero.
// Only one drain consumer may run at a time; a concurrent caller returns zero.
size_t drain(Event*, size_t) noexcept;
size_t pending() noexcept;
uint64_t dropped() noexcept;
uint64_t current_thread_dropped() noexcept;
uint64_t registry_dropped() noexcept;
size_t registered_threads() noexcept;
size_t snapshot_thread_drops(uint64_t*, size_t) noexcept;
void reset_for_test() noexcept;

uint64_t CurrentCallbackRegistrationId() noexcept;

class CallbackScope final {
 public:
  explicit CallbackScope(uint64_t callback_registration_id) noexcept;
  ~CallbackScope() noexcept;

  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  uint64_t previous_callback_registration_id_;
};

class Scope final {
 public:
  Scope(EventKind enter, EventKind exit, uint64_t token = 0,
        uint32_t topic_hash = 0, uint16_t driver_hash = 0) noexcept;
  ~Scope() noexcept;

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  EventKind enter_;
  EventKind exit_;
  uint64_t token_;
  uint64_t start_timestamp_ns_;
  uint32_t topic_hash_;
  uint16_t driver_hash_;
  size_t ring_index_;
  bool active_;
};
#endif

}  // namespace ibmw::runtime::core::observability::comm_trace

#define IBMW_COMM_TRACE_CONCAT_INNER_(left, right) left##right
#define IBMW_COMM_TRACE_CONCAT_(left, right) \
  IBMW_COMM_TRACE_CONCAT_INNER_(left, right)

#ifdef IBMW_ENABLE_COMM_TRACE
#define IBMW_COMM_TRACE_INITIALIZE()                                      \
  ::ibmw::runtime::core::observability::comm_trace::InitializeFromEnvironment()
#define IBMW_COMM_TRACE_SCOPE(enter, exit)                                \
  ::ibmw::runtime::core::observability::comm_trace::Scope                 \
      IBMW_COMM_TRACE_CONCAT_(_ibmw_comm_trace_scope_, __LINE__)(         \
          ::ibmw::runtime::core::observability::comm_trace::EventKind::enter, \
          ::ibmw::runtime::core::observability::comm_trace::EventKind::exit)
#define IBMW_COMM_TRACE_CALLBACK_SCOPE()                                  \
  ::ibmw::runtime::core::observability::comm_trace::Scope                 \
      IBMW_COMM_TRACE_CONCAT_(_ibmw_comm_trace_scope_, __LINE__)(         \
          ::ibmw::runtime::core::observability::comm_trace::EventKind::callback_enter, \
          ::ibmw::runtime::core::observability::comm_trace::EventKind::callback_exit, \
          ::ibmw::runtime::core::observability::comm_trace::CurrentCallbackRegistrationId())
#define IBMW_COMM_TRACE_CALLBACK_REGISTRATION_SCOPE(callback_registration_id) \
  ::ibmw::runtime::core::observability::comm_trace::CallbackScope         \
      IBMW_COMM_TRACE_CONCAT_(_ibmw_callback_scope_, __LINE__)(           \
          callback_registration_id)
#else
#define IBMW_COMM_TRACE_INITIALIZE() ((void)0)
#define IBMW_COMM_TRACE_SCOPE(enter, exit) ((void)0)
#define IBMW_COMM_TRACE_CALLBACK_SCOPE() ((void)0)
#define IBMW_COMM_TRACE_CALLBACK_REGISTRATION_SCOPE(callback_registration_id) \
  ((void)0)
#endif
