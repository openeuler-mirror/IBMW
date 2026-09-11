#include "observability/core_metrics.h"

#include <limits>

namespace ibmw::runtime::core::observability {

struct Registry::ReporterSlot {
  std::string kind;
  std::string metric;
  std::string topic;
  std::string type;
  std::string driver;
  std::atomic<uint64_t> value{0};
};

struct Registry::ReporterState {
  struct Transaction {
    Registry* const owner;
    const uint64_t generation;
    const uint64_t nonce;
    bool active = true;
    std::vector<std::unique_ptr<ReporterSlot>> slots;
    Transaction(Registry* owner_in, uint64_t generation_in, uint64_t nonce_in)
        : owner(owner_in), generation(generation_in), nonce(nonce_in) {}
  };
  std::vector<ReporterSlot*> slots;
  std::vector<std::unique_ptr<ReporterSlot>> storage;
  std::vector<std::unique_ptr<Transaction>> transaction_storage;
  size_t staged_count = 0;
  uint64_t generation = 0;
  uint64_t next_nonce = 0;
  std::unordered_set<Transaction*> transactions;
};

namespace {
std::string ReporterString(ibmw_reporter_string_view_v1 value) {
  return value.data && value.size
             ? std::string(value.data, static_cast<size_t>(value.size))
             : std::string();
}

std::unique_ptr<Registry::ReporterSlot> MakeReporterSlot(
    const ibmw_reporter_slot_descriptor_v1* descriptor) {
  if (!descriptor || descriptor->abi_version != IBMW_REPORTER_ABI_VERSION_V1 ||
      descriptor->struct_size <
          offsetof(ibmw_reporter_slot_descriptor_v1, reserved)) return {};
  auto slot = std::make_unique<Registry::ReporterSlot>();
  slot->kind = ReporterString(descriptor->kind);
  slot->metric = ReporterString(descriptor->metric);
  slot->topic = ReporterString(descriptor->topic);
  slot->type = ReporterString(descriptor->type);
  slot->driver = ReporterString(descriptor->driver);
  return slot->kind.empty() || slot->metric.empty() ? nullptr : std::move(slot);
}

std::string Json(std::string_view value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + '"';
}
}  // namespace

void Registry::ResetReporterLocked() {
  if (!reporter_state_) return;
  auto& state = *reporter_state_;
  state.slots.clear();
  for (auto* pending : state.transactions) {
    for (auto& slot : pending->slots) state.storage.push_back(std::move(slot));
    state.staged_count -= pending->slots.size();
    pending->slots.clear();
    pending->active = false;
  }
  state.transactions.clear();
  ++state.generation;
}

void Registry::DestroyReporter() noexcept {
  delete reporter_state_;
  reporter_state_ = nullptr;
}

Registry::ReporterSlot* Registry::RegisterReporterSlot(
    const ibmw_reporter_slot_descriptor_v1* descriptor) {
  auto slot = MakeReporterSlot(descriptor);
  if (!slot) return nullptr;
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  if (!reporter_state_) reporter_state_ = new ReporterState;
  auto& state = *reporter_state_;
  state.storage.reserve(state.storage.size() + state.staged_count + 1);
  state.slots.reserve(state.slots.size() + state.staged_count + 1);
  auto* result = slot.get();
  state.storage.push_back(std::move(slot));
  state.slots.push_back(result);
  return result;
}

bool Registry::PrepareReporterBatch(
    const ibmw_reporter_slot_descriptor_v1* descriptors, size_t count,
    ibmw_reporter_slot_v1** slots,
    ibmw_reporter_batch_transaction_v1** transaction) {
  if (!slots || !transaction) return false;
  *transaction = nullptr;
  for (size_t i = 0; i < count; ++i) slots[i] = nullptr;
  if (!descriptors || count == 0) return false;
  std::lock_guard lock(mutex_);
  if (frozen_) return false;
  if (!reporter_state_) reporter_state_ = new ReporterState;
  auto& state = *reporter_state_;
  if (state.next_nonce == std::numeric_limits<uint64_t>::max()) return false;
  auto pending = std::make_unique<ReporterState::Transaction>(
      this, state.generation, state.next_nonce + 1);
  pending->slots.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto slot = MakeReporterSlot(&descriptors[i]);
    if (!slot) return false;
    pending->slots.push_back(std::move(slot));
  }
  state.storage.reserve(state.storage.size() + state.staged_count + count);
  state.slots.reserve(state.slots.size() + state.staged_count + count);
  state.transaction_storage.reserve(state.transaction_storage.size() + 1);
  state.transactions.reserve(state.transactions.size() + 1);
  auto* raw = pending.get();
  state.transaction_storage.push_back(std::move(pending));
  state.transactions.insert(raw);
  ++state.next_nonce;
  state.staged_count += count;
  for (size_t i = 0; i < count; ++i)
    slots[i] = reinterpret_cast<ibmw_reporter_slot_v1*>(raw->slots[i].get());
  *transaction = reinterpret_cast<ibmw_reporter_batch_transaction_v1*>(raw);
  return true;
}

bool Registry::CommitReporterBatch(ibmw_reporter_batch_transaction_v1* token) {
  std::lock_guard lock(mutex_);
  if (!reporter_state_ || !token) return false;
  auto& state = *reporter_state_;
  auto* pending = reinterpret_cast<ReporterState::Transaction*>(token);
  auto found = state.transactions.find(pending);
  if (frozen_ || found == state.transactions.end() || pending->owner != this ||
      !pending->active || pending->generation != state.generation) return false;
  const size_t count = pending->slots.size();
  for (auto& slot : pending->slots) {
    auto* raw = slot.get();
    state.storage.push_back(std::move(slot));
    state.slots.push_back(raw);
  }
  state.staged_count -= count;
  state.transactions.erase(found);
  pending->active = false;
  return true;
}

void Registry::AbortReporterBatch(ibmw_reporter_batch_transaction_v1* token) noexcept {
  std::lock_guard lock(mutex_);
  if (!reporter_state_ || !token) return;
  auto& state = *reporter_state_;
  auto* pending = reinterpret_cast<ReporterState::Transaction*>(token);
  auto found = state.transactions.find(pending);
  if (found == state.transactions.end() || pending->owner != this ||
      !pending->active) return;
  state.transactions.erase(found);
  if (pending->generation == state.generation)
    state.staged_count -= pending->slots.size();
  pending->active = false;
  pending->slots.clear();
}

bool Registry::HasPendingReporterBatches() const noexcept {
  std::lock_guard lock(mutex_);
  return reporter_state_ && !reporter_state_->transactions.empty();
}

uint64_t Registry::ReporterValueForTesting(
    const ReporterSlot* slot) const noexcept {
  return slot ? slot->value.load(std::memory_order_relaxed) : 0;
}

void Registry::AppendReporterJsonlLocked(std::ostringstream& out,
                                         uint64_t& sequence) const {
  if (!reporter_state_) return;
  for (const auto* slot : reporter_state_->slots) {
    const auto value = slot->value.load(std::memory_order_relaxed);
    out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":"
        << sequence++ << ",\"kind\":" << Json(slot->kind)
        << ",\"metric\":" << Json(slot->metric)
        << ",\"topic\":" << Json(slot->topic)
        << ",\"type\":" << Json(slot->type)
        << ",\"driver\":" << Json(slot->driver) << ",\"value\":" << value
        << ",\"cumulative\":{\"metric\":" << Json(slot->metric)
        << ",\"topic\":" << Json(slot->topic)
        << ",\"type\":" << Json(slot->type)
        << ",\"driver\":" << Json(slot->driver) << ",\"value\":" << value
        << "},\"complete\":true}\n";
  }
}

size_t Registry::ReporterStorageSizeLocked() const noexcept {
  return reporter_state_ ? reporter_state_->storage.size() : 0;
}

Registry::ReporterSlot* Controller::RegisterReporterSlot(
    const ibmw_reporter_slot_descriptor_v1* descriptor) noexcept {
  try {
    std::lock_guard lock(mutex_);
    if (state_ != State::kInitialized || !Enabled()) return nullptr;
    return runtime_.registry().RegisterReporterSlot(descriptor);
  } catch (...) {
    return nullptr;
  }
}
bool Controller::PrepareReporterBatch(
    const ibmw_reporter_slot_descriptor_v1* descriptors, size_t count,
    ibmw_reporter_slot_v1** slots,
    ibmw_reporter_batch_transaction_v1** transaction) noexcept {
  try {
    std::lock_guard lock(mutex_);
    return state_ == State::kInitialized && Enabled() &&
           runtime_.registry().PrepareReporterBatch(descriptors, count, slots,
                                                    transaction);
  } catch (...) {
    return false;
  }
}
bool Controller::CommitReporterBatch(
    ibmw_reporter_batch_transaction_v1* transaction) noexcept {
  try {
    std::lock_guard lock(mutex_);
    return state_ == State::kInitialized && Enabled() &&
           runtime_.registry().CommitReporterBatch(transaction);
  } catch (...) {
    return false;
  }
}
void Controller::AbortReporterBatch(
    ibmw_reporter_batch_transaction_v1* transaction) noexcept {
  try {
    std::lock_guard lock(mutex_);
    runtime_.registry().AbortReporterBatch(transaction);
  } catch (...) {
  }
}

}  // namespace ibmw::runtime::core::observability

namespace {
using ibmw::runtime::core::observability::Controller;
using ibmw::runtime::core::observability::Registry;
ibmw_reporter_slot_v1* ReporterRegister(
    void* owner, const ibmw_reporter_slot_descriptor_v1* descriptor) {
  return reinterpret_cast<ibmw_reporter_slot_v1*>(
      static_cast<Controller*>(owner)->RegisterReporterSlot(descriptor));
}
void ReporterAdd(void*, ibmw_reporter_slot_v1* slot, uint64_t value) {
  if (slot)
    reinterpret_cast<Registry::ReporterSlot*>(slot)->value.fetch_add(
        value, std::memory_order_relaxed);
}
void ReporterAddSigned(void*, ibmw_reporter_slot_v1* slot, int64_t delta) {
  if (!slot) return;
  auto& value = reinterpret_cast<Registry::ReporterSlot*>(slot)->value;
  if (delta >= 0) {
    value.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed);
    return;
  }
  const uint64_t decrement = static_cast<uint64_t>(-(delta + 1)) + 1;
  uint64_t current = value.load(std::memory_order_relaxed);
  while (!value.compare_exchange_weak(
      current, current < decrement ? 0 : current - decrement,
      std::memory_order_relaxed)) {}
}
int ReporterBatchPrepare(
    void* owner, const ibmw_reporter_slot_descriptor_v1* descriptors,
    size_t count, ibmw_reporter_slot_v1** slots,
    ibmw_reporter_batch_transaction_v1** transaction) {
  auto& controller = ibmw::runtime::core::observability::CoreMetricsController();
  return owner == &controller && controller.PrepareReporterBatch(
                                     descriptors, count, slots, transaction);
}
int ReporterBatchCommit(void* owner,
                        ibmw_reporter_batch_transaction_v1* transaction) {
  auto& controller = ibmw::runtime::core::observability::CoreMetricsController();
  return owner == &controller && controller.CommitReporterBatch(transaction);
}
void ReporterBatchAbort(void* owner,
                        ibmw_reporter_batch_transaction_v1* transaction) {
  auto& controller = ibmw::runtime::core::observability::CoreMetricsController();
  if (owner == &controller) controller.AbortReporterBatch(transaction);
}
}  // namespace

extern "C" const ibmw_reporter_v1* ibmw_reporter_query_enabled_v1(void) {
  auto& controller = ibmw::runtime::core::observability::CoreMetricsController();
  if (!controller.ReporterAvailable()) return nullptr;
  static const ibmw_reporter_v1 api{
      IBMW_REPORTER_ABI_VERSION_V1, sizeof(ibmw_reporter_v1), &controller,
      &ReporterRegister, &ReporterAdd, {0}, &ReporterAddSigned};
  return &api;
}
extern "C" const ibmw_reporter_batch_v1*
ibmw_reporter_query_batch_enabled_v1(void) {
  auto& controller = ibmw::runtime::core::observability::CoreMetricsController();
  if (!controller.ReporterAvailable()) return nullptr;
  static const ibmw_reporter_batch_v1 api{
      IBMW_REPORTER_BATCH_ABI_VERSION_V1, sizeof(ibmw_reporter_batch_v1),
      &controller, &ReporterBatchPrepare, &ReporterBatchCommit,
      &ReporterBatchAbort};
  return &api;
}
