#include "observability/comm_trace.h"

#include "engine/transport/internal/comm_trace_send_overlay.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace ibmw::runtime::core::observability::comm_trace {
namespace {

constexpr size_t kInvalidRing = kMaxThreadRings;
constexpr size_t kNoRingAvailable = kMaxThreadRings + 1;

enum class SlotState : uint8_t {
  free = 0,
  initializing = 1,
  active = 2,
  retired = 3
};

static_assert(std::atomic<uint8_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<SlotState>::is_always_lock_free);
static_assert(std::atomic<RuntimeMode>::is_always_lock_free);
static_assert(std::atomic<ClockForTest>::is_always_lock_free);

struct ThreadRing final {
  std::array<Event, kCapacity> events{};
  std::atomic<uint64_t> read_position{0};
  std::atomic<uint64_t> write_position{0};
  std::atomic<uint64_t> drop_count{0};
  std::atomic<uint64_t> generation{0};
  std::atomic<SlotState> state{SlotState::free};
};

struct ThreadState final {
  size_t ring_index{kInvalidRing};
  uint64_t ring_generation{0};
  uint64_t registry_epoch{0};
  uint64_t zero_token_sample_sequence{0};
  uint64_t unregistered_drop_count{0};

  ~ThreadState() noexcept;
};

std::array<ThreadRing, kMaxThreadRings> rings;
std::atomic<uint64_t> total_drop_count{0};
std::atomic<uint64_t> registry_drop_count{0};
std::atomic<uint64_t> registry_epoch{1};
std::atomic<RuntimeMode> runtime_mode{RuntimeMode::off};
std::atomic_flag registry_and_drain_guard = ATOMIC_FLAG_INIT;
thread_local uint64_t current_callback_registration_id = 0;

class BlockingGuard final {
 public:
  BlockingGuard() noexcept {
    while (registry_and_drain_guard.test_and_set(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  ~BlockingGuard() noexcept {
    registry_and_drain_guard.clear(std::memory_order_release);
  }
};

class TryGuard final {
 public:
  TryGuard() noexcept
      : acquired_(!registry_and_drain_guard.test_and_set(
            std::memory_order_acquire)) {}
  ~TryGuard() noexcept {
    if (acquired_) registry_and_drain_guard.clear(std::memory_order_release);
  }
  explicit operator bool() const noexcept { return acquired_; }

 private:
  bool acquired_;
};

uint64_t SteadyClockNanoseconds() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::atomic<ClockForTest> clock_source{&SteadyClockNanoseconds};

RuntimeMode ParseMode(const char* value) noexcept {
  if (value == nullptr) return RuntimeMode::off;
  if (std::strcmp(value, "full") == 0) return RuntimeMode::full;
  if (std::strcmp(value, "sampled") == 0) return RuntimeMode::sampled;
  return RuntimeMode::off;
}

uint64_t MixToken(uint64_t value) noexcept {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

ThreadState& LocalState() noexcept {
  thread_local ThreadState state;
  const uint64_t epoch = registry_epoch.load(std::memory_order_acquire);
  if (state.registry_epoch != epoch) {
    state = ThreadState{};
    state.registry_epoch = epoch;
  }
  return state;
}

void RetireRing(ThreadState& owner) noexcept {
  if (owner.ring_index >= kMaxThreadRings) return;
  BlockingGuard guard;
  ThreadRing& ring = rings[owner.ring_index];
  if (ring.generation.load(std::memory_order_relaxed) !=
          owner.ring_generation ||
      ring.state.load(std::memory_order_acquire) != SlotState::active) {
    return;
  }
  ring.state.store(SlotState::retired, std::memory_order_release);
  const uint64_t read = ring.read_position.load(std::memory_order_acquire);
  const uint64_t write = ring.write_position.load(std::memory_order_acquire);
  if (read == write) {
    ring.state.store(SlotState::free, std::memory_order_release);
  }
}

ThreadState::~ThreadState() noexcept {
  if (registry_epoch == ::ibmw::runtime::core::observability::comm_trace::
                            registry_epoch.load(std::memory_order_acquire)) {
    RetireRing(*this);
  }
}

bool ShouldRecord(RuntimeMode current_mode, uint64_t token) noexcept {
  if (current_mode == RuntimeMode::full) return true;
  if (current_mode != RuntimeMode::sampled) return false;
  if (token == 0) token = LocalState().zero_token_sample_sequence++;
  return (MixToken(token) & 3U) == 0;
}

size_t AcquireRing(ThreadState& state) noexcept {
  if (state.ring_index < kMaxThreadRings) return state.ring_index;
  state.ring_index = kInvalidRing;
  BlockingGuard guard;
  for (size_t i = 0; i < kMaxThreadRings; ++i) {
    SlotState expected = SlotState::free;
    if (rings[i].state.compare_exchange_strong(
            expected, SlotState::initializing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      ThreadRing& ring = rings[i];
      const uint64_t generation =
          ring.generation.fetch_add(1, std::memory_order_relaxed) + 1;
      ring.read_position.store(0, std::memory_order_relaxed);
      ring.write_position.store(0, std::memory_order_relaxed);
      ring.drop_count.store(0, std::memory_order_relaxed);
      state.ring_index = i;
      state.ring_generation = generation;
      ring.state.store(SlotState::active, std::memory_order_release);
      return i;
    }
  }
  state.ring_index = kNoRingAvailable;
  return state.ring_index;
}

void RecordDrop(ThreadState& state, ThreadRing* ring) noexcept {
  total_drop_count.fetch_add(1, std::memory_order_relaxed);
  if (ring != nullptr) {
    ring->drop_count.fetch_add(1, std::memory_order_relaxed);
  } else {
    ++state.unregistered_drop_count;
    registry_drop_count.fetch_add(1, std::memory_order_relaxed);
  }
}

bool HasCapacity(const ThreadRing& ring, uint64_t write_position,
                 size_t required) noexcept {
  const uint64_t read_position =
      ring.read_position.load(std::memory_order_acquire);
  return write_position >= read_position &&
         write_position - read_position <= kCapacity - required;
}

bool IsValidPair(EventKind enter, EventKind exit) noexcept {
  return (enter == EventKind::publish_enter &&
          exit == EventKind::publish_return) ||
         (enter == EventKind::callback_enter &&
          exit == EventKind::callback_exit);
}

void EnqueueSpan(size_t ring_index, EventKind enter, EventKind exit,
                 uint64_t start_timestamp_ns, uint64_t token,
                 uint32_t topic_hash, uint16_t driver_hash) noexcept {
  ThreadState& state = LocalState();
  ThreadRing& ring = rings[ring_index];
  if (!IsValidPair(enter, exit)) {
    RecordDrop(state, &ring);
    return;
  }
  const uint64_t write_position =
      ring.write_position.load(std::memory_order_relaxed);
  if (!HasCapacity(ring, write_position, 2)) {
    RecordDrop(state, &ring);
    return;
  }
  const uint64_t end_timestamp_ns =
      clock_source.load(std::memory_order_relaxed)();
  ring.events[write_position % kCapacity] =
      Event{start_timestamp_ns, token, topic_hash, driver_hash, enter, {}};
  ring.events[(write_position + 1) % kCapacity] =
      Event{end_timestamp_ns, token, topic_hash, driver_hash, exit, {}};
  ring.write_position.store(write_position + 2, std::memory_order_release);
}

}  // namespace

void InitializeFromEnvironment() noexcept {
  runtime_mode.store(ParseMode(std::getenv("IBMW_COMM_TRACE")),
                     std::memory_order_release);
}

RuntimeMode mode() noexcept {
  return runtime_mode.load(std::memory_order_relaxed);
}

void SetModeForTest(RuntimeMode value) noexcept {
  runtime_mode.store(value, std::memory_order_release);
}

void SetClockForTest(ClockForTest clock) noexcept {
  clock_source.store(clock ? clock : &SteadyClockNanoseconds,
                     std::memory_order_release);
}

transport::internal::PublishEnvelopeV1 CapturePublishEnvelope() noexcept {
  if (mode() == RuntimeMode::off) {
    return transport::internal::InvalidPublishEnvelopeV1();
  }
  return transport::internal::PublishEnvelopeV1{
      transport::internal::kPublishEnvelopeAbiV1,
      sizeof(transport::internal::PublishEnvelopeV1),
      transport::internal::kPublishEnvelopeValid,
      transport::internal::kPublishPathSendAccess,
      clock_source.load(std::memory_order_relaxed)(),
      0};
}

void Emit(EventKind kind, uint64_t token, uint32_t topic_hash,
           uint16_t driver_hash) noexcept {
  const RuntimeMode current_mode = mode();
  if (!ShouldRecord(current_mode, token)) return;
  ThreadState& state = LocalState();
  const size_t ring_index = AcquireRing(state);
  if (ring_index >= kMaxThreadRings) {
    RecordDrop(state, nullptr);
    return;
  }
  const bool publish = kind == EventKind::publish_enter ||
                       kind == EventKind::publish_return;
  EnqueueSpan(ring_index,
              publish ? EventKind::publish_enter : EventKind::callback_enter,
              publish ? EventKind::publish_return : EventKind::callback_exit,
              clock_source.load(std::memory_order_relaxed)(), token, topic_hash,
              driver_hash);
}

size_t drain(Event* output, size_t output_capacity) noexcept {
  if (output == nullptr || output_capacity < 2) return 0;
  TryGuard guard;
  if (!guard) return 0;
  size_t count = 0;
  for (ThreadRing& ring : rings) {
    if (output_capacity - count < 2) break;
    const SlotState state = ring.state.load(std::memory_order_acquire);
    if (state != SlotState::active && state != SlotState::retired) continue;
    uint64_t read_position =
        ring.read_position.load(std::memory_order_relaxed);
    const uint64_t write_position =
        ring.write_position.load(std::memory_order_acquire);
    while (output_capacity - count >= 2 &&
           write_position - read_position >= 2) {
      const Event& enter = ring.events[read_position % kCapacity];
      const Event& exit = ring.events[(read_position + 1) % kCapacity];
      const bool valid_pair =
          enter.token == exit.token && enter.topic_hash == exit.topic_hash &&
          enter.driver_hash == exit.driver_hash &&
          IsValidPair(enter.kind, exit.kind);
      if (!valid_pair) break;
      output[count++] = enter;
      output[count++] = exit;
      read_position += 2;
    }
    ring.read_position.store(read_position, std::memory_order_release);
    if (state == SlotState::retired && read_position == write_position) {
      ring.state.store(SlotState::free, std::memory_order_release);
    }
  }
  return count;
}

size_t pending() noexcept {
  BlockingGuard guard;
  size_t total = 0;
  for (const ThreadRing& ring : rings) {
    const SlotState state = ring.state.load(std::memory_order_acquire);
    if (state != SlotState::active && state != SlotState::retired) continue;
    const uint64_t read_position =
        ring.read_position.load(std::memory_order_acquire);
    const uint64_t write_position =
        ring.write_position.load(std::memory_order_acquire);
    if (write_position >= read_position) {
      const uint64_t count = write_position - read_position;
      total += static_cast<size_t>(count > kCapacity ? kCapacity : count);
    }
  }
  return total;
}

uint64_t dropped() noexcept {
  return total_drop_count.load(std::memory_order_relaxed);
}

uint64_t current_thread_dropped() noexcept {
  ThreadState& state = LocalState();
  if (state.ring_index < kMaxThreadRings) {
    return rings[state.ring_index].drop_count.load(std::memory_order_relaxed);
  }
  return state.unregistered_drop_count;
}

uint64_t registry_dropped() noexcept {
  return registry_drop_count.load(std::memory_order_relaxed);
}

size_t registered_threads() noexcept {
  BlockingGuard guard;
  size_t count = 0;
  for (const ThreadRing& ring : rings) {
    const SlotState state = ring.state.load(std::memory_order_acquire);
    if (state == SlotState::active || state == SlotState::retired) ++count;
  }
  return count;
}

size_t snapshot_thread_drops(uint64_t* output,
                             size_t output_capacity) noexcept {
  if (output == nullptr) return 0;
  BlockingGuard guard;
  size_t count = 0;
  for (const ThreadRing& ring : rings) {
    if (count == output_capacity) break;
    const SlotState state = ring.state.load(std::memory_order_acquire);
    if (state != SlotState::active && state != SlotState::retired) continue;
    output[count++] = ring.drop_count.load(std::memory_order_relaxed);
  }
  return count;
}

void reset_for_test() noexcept {
  BlockingGuard guard;
  total_drop_count.store(0, std::memory_order_relaxed);
  registry_drop_count.store(0, std::memory_order_relaxed);
  runtime_mode.store(RuntimeMode::off, std::memory_order_relaxed);
  clock_source.store(&SteadyClockNanoseconds, std::memory_order_relaxed);
  for (ThreadRing& ring : rings) {
    ring.read_position.store(0, std::memory_order_relaxed);
    ring.write_position.store(0, std::memory_order_relaxed);
    ring.drop_count.store(0, std::memory_order_relaxed);
    ring.generation.fetch_add(1, std::memory_order_relaxed);
    ring.state.store(SlotState::free, std::memory_order_relaxed);
  }
  registry_epoch.fetch_add(1, std::memory_order_release);
}

uint64_t CurrentCallbackRegistrationId() noexcept {
  return current_callback_registration_id;
}

CallbackScope::CallbackScope(uint64_t callback_registration_id) noexcept
    : previous_callback_registration_id_(current_callback_registration_id) {
  current_callback_registration_id = callback_registration_id;
}

CallbackScope::~CallbackScope() noexcept {
  current_callback_registration_id = previous_callback_registration_id_;
}

Scope::Scope(EventKind enter, EventKind exit, uint64_t token,
             uint32_t topic_hash, uint16_t driver_hash) noexcept
    : enter_(enter),
      exit_(exit),
      token_(token),
      start_timestamp_ns_(0),
      topic_hash_(topic_hash),
      driver_hash_(driver_hash),
      ring_index_(kInvalidRing),
      active_(false) {
  const RuntimeMode current_mode = mode();
  if (!ShouldRecord(current_mode, token)) return;
  ThreadState& state = LocalState();
  ring_index_ = AcquireRing(state);
  if (ring_index_ >= kMaxThreadRings) {
    RecordDrop(state, nullptr);
    return;
  }
  start_timestamp_ns_ = clock_source.load(std::memory_order_relaxed)();
  active_ = true;
}

Scope::~Scope() noexcept {
  if (active_) {
    EnqueueSpan(ring_index_, enter_, exit_, start_timestamp_ns_, token_,
                topic_hash_, driver_hash_);
  }
}

}  // namespace ibmw::runtime::core::observability::comm_trace
