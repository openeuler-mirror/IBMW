// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "observability/core_metrics.h"
#include "engine/transport/transport_catalog.h"
#include "engine/transport/transport_driver.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <chrono>
#include <unordered_set>
#include <iterator>
namespace ibmw::runtime::core::observability {
namespace {
std::string Key(std::string_view first, std::string_view second, std::string_view third = {}) {
  return std::string(first) + '\x1f' + std::string(second) + '\x1f' + std::string(third);
}
std::string FrameworkKey(std::string_view pkg, std::string_view module, std::string_view topic,
                         std::string_view type) {
  return Key(pkg, module, topic) + '\x1f' + std::string(type);
}
std::string Json(std::string_view text) {
  std::string out = "\"";
  for (unsigned char byte : text) {
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20) {
          static constexpr char kHexDigits[] = "0123456789abcdef";
          out += "\\u00";
          out += kHexDigits[byte >> 4];
          out += kHexDigits[byte & 15];
        } else {
          out += static_cast<char>(byte);
        }
    }
  }
  return out + '"';
}
}  // namespace
Registry::Registry() = default;
Registry::~Registry() { DestroyReporter(); }
void Registry::Reset() {
  std::lock_guard lock(mutex_);
  logical_slots_.clear();
  driver_slots_.clear();
  pod_logical_slots_.clear();
  pod_driver_slots_.clear();
  loaned_receive_slots_.clear();
  logical_storage_.clear();
  driver_storage_.clear();
  pod_logical_storage_.clear();
  pod_driver_storage_.clear();
  ResetReporterLocked();
  logical_.clear();
  drivers_.clear();
  loan_drivers_.clear();
  pod_logical_.clear();
  pod_drivers_.clear();
  frozen_.store(false, std::memory_order_release);
}
ObservationMode Runtime::ModeFromEnvironment() {
  const char* value = std::getenv("IBMW_CORE_METRICS");
  return value && std::strcmp(value, "counters") == 0 ? ObservationMode::kCounters
                                                      : ObservationMode::kOff;
}
Registry::LogicalSlot* Registry::RegisterLogicalSlot(std::string_view topic, std::string_view type,
                                                     std::string_view pkg,
                                                     std::string_view module) {
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  auto key = FrameworkKey(pkg, module, topic, type);
  if (auto found = logical_.find(key); found != logical_.end()) return found->second;
  auto slot = std::make_unique<LogicalSlot>(std::string(pkg), std::string(module),
                                            std::string(topic), std::string(type));
  auto slot_ptr = slot.get();
  logical_storage_.push_back(std::move(slot));
  logical_slots_.push_back(slot_ptr);
  logical_.emplace(std::move(key), slot_ptr);
  return slot_ptr;
}
Registry::DriverSlot* Registry::RegisterDriverSlot(std::string_view topic, std::string_view type,
                                                   std::string_view driver, std::string_view pkg,
                                                   std::string_view module) {
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  auto key = FrameworkKey(pkg, module, topic, type) + '\x1f' + std::string(driver);
  if (auto found = drivers_.find(key); found != drivers_.end()) return found->second;
  auto slot =
      std::make_unique<DriverSlot>(std::string(pkg), std::string(module), std::string(topic),
                                   std::string(type), std::string(driver));
  auto slot_ptr = slot.get();
  driver_storage_.push_back(std::move(slot));
  driver_slots_.push_back(slot_ptr);
  drivers_.emplace(std::move(key), slot_ptr);
  return slot_ptr;
}
Registry::DriverSlot* Registry::RegisterLoanDriverSlot(std::string_view driver) {
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  auto key = std::string(driver);
  if (auto found = loan_drivers_.find(key); found != loan_drivers_.end()) return found->second;
  auto slot = std::make_unique<DriverSlot>("", "", "", "", std::string(driver));
  auto slot_ptr = slot.get();
  driver_storage_.push_back(std::move(slot));
  driver_slots_.push_back(slot_ptr);
  loan_drivers_.emplace(std::move(key), slot_ptr);
  return slot_ptr;
}
Registry::RegistrationMark Registry::MarkRegistration() const {
  std::lock_guard lock(mutex_);
  return {logical_storage_.size(), driver_storage_.size()};
}
void Registry::RollbackRegistration(RegistrationMark mark) noexcept {
  std::lock_guard lock(mutex_);
  if (mark.logical > logical_storage_.size() || mark.driver > driver_storage_.size()) return;
  for (size_t i = mark.logical; i < logical_storage_.size(); ++i) {
    auto* slot = logical_storage_[i].get();
    for (auto it = logical_.begin(); it != logical_.end();)
      it = it->second == slot ? logical_.erase(it) : std::next(it);
  }
  for (size_t i = mark.driver; i < driver_storage_.size(); ++i) {
    auto* slot = driver_storage_[i].get();
    for (auto it = drivers_.begin(); it != drivers_.end();)
      it = it->second == slot ? drivers_.erase(it) : std::next(it);
    for (auto it = loan_drivers_.begin(); it != loan_drivers_.end();)
      it = it->second == slot ? loan_drivers_.erase(it) : std::next(it);
  }
  logical_slots_.resize(mark.logical);
  logical_storage_.resize(mark.logical);
  driver_slots_.resize(mark.driver);
  driver_storage_.resize(mark.driver);
}
Registry::PodLogicalSlot* Registry::RegisterPodLogicalSlot(std::string_view topic,
                                                           std::string_view type) {
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  auto key = Key(topic, type);
  if (auto found = pod_logical_.find(key); found != pod_logical_.end()) return found->second;
  auto slot = std::make_unique<PodLogicalSlot>(std::string(topic), std::string(type));
  auto slot_ptr = slot.get();
  pod_logical_storage_.push_back(std::move(slot));
  pod_logical_slots_.push_back(slot_ptr);
  pod_logical_.emplace(std::move(key), slot_ptr);
  return slot_ptr;
}
Registry::PodDriverSlot* Registry::RegisterPodDriverSlot(std::string_view topic,
                                                         std::string_view type,
                                                         std::string_view driver) {
  std::lock_guard lock(mutex_);
  if (frozen_) return nullptr;
  auto key = Key(topic, type, driver);
  if (auto found = pod_drivers_.find(key); found != pod_drivers_.end()) return found->second;
  auto slot =
      std::make_unique<PodDriverSlot>(std::string(topic), std::string(type), std::string(driver));
  auto slot_ptr = slot.get();
  pod_driver_storage_.push_back(std::move(slot));
  pod_driver_slots_.push_back(slot_ptr);
  pod_drivers_.emplace(std::move(key), slot_ptr);
  return slot_ptr;
}
std::shared_ptr<Registry::LoanedReceiveSlot> Registry::RegisterLoanedReceiveSlot(
    std::string_view topic, std::string_view type, std::string_view driver) {
  std::lock_guard lock(mutex_);
  if (frozen_) return {};
  auto slot = std::make_shared<LoanedReceiveSlot>(std::string(topic), std::string(type),
                                                  std::string(driver));
  loaned_receive_slots_.push_back(slot);
  return slot;
}
bool Registry::RegisterLogical(std::string_view topic, std::string_view type) {
  return RegisterLogicalSlot(topic, type) != nullptr;
}
bool Registry::RegisterDriver(std::string_view topic, std::string_view type,
                              std::string_view driver) {
  return RegisterDriverSlot(topic, type, driver) != nullptr;
}
void Registry::Freeze() {
  std::lock_guard lock(mutex_);
  frozen_.store(true, std::memory_order_release);
}
void Registry::AddLogicalAttempt(LogicalSlot* slot) noexcept {
  if (slot) slot->attempted.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLogicalRejected(LogicalSlot* slot) noexcept {
  if (slot) slot->rejected.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddSendAttempt(DriverSlot* slot) noexcept {
  if (slot) slot->attempted.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddSendReturned(DriverSlot* slot) noexcept {
  if (slot) slot->returned.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLoanAttempt(LogicalSlot* slot) noexcept {
  if (slot) slot->loan_attempted.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLoanSucceeded(LogicalSlot* slot) noexcept {
  if (slot) slot->loan_succeeded.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLoanFailed(LogicalSlot* slot) noexcept {
  if (slot) slot->loan_failed.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLoanPublishCalled(DriverSlot* slot) noexcept {
  if (slot) slot->loan_publish_called.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLoanReleaseCalled(DriverSlot* slot) noexcept {
  if (slot) slot->loan_release_called.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddPodLoanAttempt(PodLogicalSlot* slot) noexcept {
  if (slot) slot->attempted.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddPodLoanSucceeded(PodLogicalSlot* slot) noexcept {
  if (slot) slot->succeeded.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddPodLoanFailed(PodLogicalSlot* slot) noexcept {
  if (slot) slot->failed.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddPodLoanPublishCalled(PodDriverSlot* slot) noexcept {
  if (slot) slot->publish_called.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddPodLoanReleaseCalled(PodDriverSlot* slot) noexcept {
  if (slot) slot->release_called.fetch_add(1, std::memory_order_relaxed);
}
void Registry::AddLogicalAttempt(std::string_view topic, std::string_view type) {
  std::lock_guard lock(mutex_);
  auto found = logical_.find(FrameworkKey("", "", topic, type));
  if (found != logical_.end()) AddLogicalAttempt(found->second);
}
void Registry::AddLogicalRejected(std::string_view topic, std::string_view type) {
  std::lock_guard lock(mutex_);
  auto found = logical_.find(FrameworkKey("", "", topic, type));
  if (found != logical_.end()) AddLogicalRejected(found->second);
}
void Registry::AddSendAttempt(std::string_view topic, std::string_view type,
                              std::string_view driver) {
  std::lock_guard lock(mutex_);
  auto found = drivers_.find(FrameworkKey("", "", topic, type) + '\x1f' + std::string(driver));
  if (found != drivers_.end()) AddSendAttempt(found->second);
}
void Registry::AddSendReturned(std::string_view topic, std::string_view type,
                               std::string_view driver) {
  std::lock_guard lock(mutex_);
  auto found = drivers_.find(FrameworkKey("", "", topic, type) + '\x1f' + std::string(driver));
  if (found != drivers_.end()) AddSendReturned(found->second);
}
std::string Registry::SerializeJsonl(bool include_core, bool include_reporter,
                                     uint64_t first_sequence) const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  uint64_t sequence = first_sequence;
  if (include_core) {
    for (auto& slot : logical_slots_) {
      out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
          << ",\"kind\":\"logical_publish\",\"cumulative\":{\"pkg\":" << Json(slot->pkg)
          << ",\"module\":" << Json(slot->module) << ",\"topic\":" << Json(slot->topic)
          << ",\"type\":" << Json(slot->type) << ",\"attempted\":" << slot->attempted.load()
          << ",\"rejected\":" << slot->rejected.load() << "},\"complete\":true}\n";
      out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
          << ",\"kind\":\"logical_loan\",\"cumulative\":{\"pkg\":" << Json(slot->pkg)
          << ",\"module\":" << Json(slot->module) << ",\"topic\":" << Json(slot->topic)
          << ",\"type\":" << Json(slot->type)
          << ",\"loan_attempted\":" << slot->loan_attempted.load()
          << ",\"loan_succeeded\":" << slot->loan_succeeded.load()
          << ",\"loan_failed\":" << slot->loan_failed.load() << "},\"complete\":true}\n";
    }
    for (auto& slot : pod_logical_slots_)
      out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
          << ",\"kind\":\"logical_pod_loan\",\"cumulative\":{\"topic\":" << Json(slot->topic)
          << ",\"type\":" << Json(slot->type) << ",\"loan_attempted\":" << slot->attempted.load()
          << ",\"loan_succeeded\":" << slot->succeeded.load()
          << ",\"loan_failed\":" << slot->failed.load() << "},\"complete\":true}\n";
    for (auto& slot : pod_driver_slots_)
      out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
          << ",\"kind\":\"source_driver_pod_loan\",\"cumulative\":{\"topic\":" << Json(slot->topic)
          << ",\"type\":" << Json(slot->type) << ",\"driver\":" << Json(slot->driver)
          << ",\"loan_publish_called\":" << slot->publish_called.load()
          << ",\"loan_release_called\":" << slot->release_called.load() << "},\"complete\":true}\n";
    for (auto& slot : loaned_receive_slots_)
      out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
          << ",\"kind\":\"loaned_receive_callback\",\"cumulative\":{\"topic\":" << Json(slot->topic)
          << ",\"type\":" << Json(slot->type) << ",\"driver\":" << Json(slot->driver)
          << ",\"callback_entered\":" << slot->callback_entered.load()
          << ",\"callback_completed\":" << slot->callback_completed.load()
          << ",\"callback_threw_std\":" << slot->callback_threw_std.load()
          << ",\"callback_threw_nonstd\":" << slot->callback_threw_nonstd.load()
          << ",\"release_called\":" << slot->release_called.load() << "},\"complete\":true}\n";
    for (auto& slot : driver_slots_) {
      if (slot->topic.empty() && slot->type.empty()) {
        out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
            << ",\"kind\":\"source_driver_loan\",\"cumulative\":{\"driver\":" << Json(slot->driver)
            << ",\"loan_publish_called\":" << slot->loan_publish_called.load()
            << ",\"loan_release_called\":" << slot->loan_release_called.load()
            << "},\"complete\":true}\n";
      } else {
        out << "{\"schema\":\"ibmw.core_metrics.v1\",\"snapshot_seq\":" << sequence++
            << ",\"kind\":\"driver_send\",\"cumulative\":{\"pkg\":" << Json(slot->pkg)
            << ",\"module\":" << Json(slot->module) << ",\"topic\":" << Json(slot->topic)
            << ",\"type\":" << Json(slot->type) << ",\"driver\":" << Json(slot->driver)
            << ",\"attempted\":" << slot->attempted.load()
            << ",\"returned\":" << slot->returned.load() << "},\"complete\":true}\n";
      }
    }
  }
  if (include_reporter) AppendReporterJsonlLocked(out, sequence);
  return out.str();
}
std::string Registry::SnapshotJsonl() const { return SerializeJsonl(true, true, 1); }
std::string Registry::ReporterSnapshotJsonl(uint64_t first_sequence) const {
  return SerializeJsonl(false, true, first_sequence);
}
size_t Registry::StorageSizeForTesting() const {
  std::lock_guard lock(mutex_);
  size_t size = logical_storage_.size() + driver_storage_.size() + pod_logical_storage_.size() +
                pod_driver_storage_.size() + loaned_receive_slots_.size();
  size += ReporterStorageSizeLocked();
  return size;
}
bool Registry::WriteSnapshot(const std::string& dir) const {
  try {
    const auto path = std::filesystem::path(dir) / "metrics.jsonl";
    std::filesystem::create_directories(dir);
    const auto tmp = path.string() + ".tmp." +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
      std::ofstream file(tmp, std::ios::trunc);
      if (!file) return false;
      file << SnapshotJsonl();
      if (!file.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      std::filesystem::remove(tmp);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}
bool Runtime::FinalizeSnapshot() {
  if (mode() != ObservationMode::kCounters) return true;
  const char* output_dir = std::getenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
  return registry_.WriteSnapshot(output_dir && *output_dir ? output_dir : ".");
}
void Runtime::Initialize() {
  mode_.store(ModeFromEnvironment(), std::memory_order_release);
  registry_.Reset();
}
Controller& Controller::Instance() {
  static Controller value;
  return value;
}
namespace {
struct ReceiveRegistrationKey {
  const void* dispatcher;
  const void* registration;
  bool operator==(const ReceiveRegistrationKey& other) const noexcept {
    return dispatcher == other.dispatcher && registration == other.registration;
  }
};
struct ReceiveRegistrationHash {
  size_t operator()(const ReceiveRegistrationKey& key) const noexcept {
    return std::hash<const void*>{}(key.dispatcher) ^
           (std::hash<const void*>{}(key.registration) << 1);
  }
};
struct ControllerDrainState {
  std::unordered_set<const void*> disabled_dispatchers;
  std::unordered_set<ReceiveRegistrationKey, ReceiveRegistrationHash> draining_registrations;
};
ControllerDrainState& DrainState() {
  static ControllerDrainState state;
  return state;
}
void DisableGates(std::vector<std::shared_ptr<LoanedReceiveGate>>& gates) noexcept {
  for (auto& gate : gates)
    if (gate) gate->BeginDisable();
  for (auto& gate : gates)
    if (gate) gate->WaitDisabled();
}
}  // namespace

void Controller::Initialize(const void* identity) {
  std::vector<std::shared_ptr<LoanedReceiveGate>> gates;
  {
    std::lock_guard lock(mutex_);
    const auto lifecycle = lifecycles_.find(identity);
    if (lifecycle != lifecycles_.end() && lifecycle->second.initialized) return;
    if (state_ == State::kUninitialized || state_ == State::kStopped) {
      for (auto& [dispatcher, entries] : entries_) {
        DrainState().disabled_dispatchers.insert(dispatcher);
        for (auto& [_, entry] : entries.loaned_receive) {
          if (auto gate = entry.gate.lock()) gates.push_back(std::move(gate));
        }
      }
    } else {
      lifecycles_[identity] = {true, false};
      return;
    }
  }
  DisableGates(gates);
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kUninitialized || state_ == State::kStopped) {
      entries_.clear();
      entry_storage_.clear();
      pod_entry_storage_.clear();
      loan_drivers_.clear();
      lifecycles_.clear();
      runtime_.Initialize();
      state_ = State::kInitialized;
      DrainState().disabled_dispatchers.clear();
      DrainState().draining_registrations.clear();
      dispatcher_drains_.clear();
      cached_entry_dispatchers_.clear();
      cached_metric_gates_.clear();
    }
    lifecycles_[identity] = {true, false};
  }
}
void Controller::Start(const void* identity) {
  std::lock_guard lock(mutex_);
  auto lifecycle = lifecycles_.find(identity);
  if (lifecycle == lifecycles_.end() || !lifecycle->second.initialized || lifecycle->second.started)
    return;
  if (state_ == State::kInitialized) {
    if (runtime_.registry().HasPendingReporterBatches()) return;
    runtime_.Start();
    state_ = State::kStarted;
  }
  if (state_ == State::kStarted) {
    lifecycle->second.started = true;
    ++active_instances_;
  }
}
bool Controller::FinalizeSnapshot(const void* identity) {
  std::lock_guard lock(mutex_);
  auto lifecycle = lifecycles_.find(identity);
  if (lifecycle == lifecycles_.end()) return true;
  if (!lifecycle->second.started) {
    lifecycles_.erase(lifecycle);
    if (lifecycles_.empty() && active_instances_ == 0) state_ = State::kStopped;
    return true;
  }
  if (active_instances_ == 1 && !cached_entry_dispatchers_.empty()) return false;
  lifecycles_.erase(lifecycle);
  --active_instances_;
  if (active_instances_ != 0) return true;
  state_ = State::kStopped;
  const bool result = runtime_.FinalizeSnapshot();
  entries_.clear();
  entry_storage_.clear();
  pod_entry_storage_.clear();
  dispatcher_drains_.clear();
  cached_entry_dispatchers_.clear();
  loan_drivers_.clear();
  lifecycles_.clear();
  DrainState().disabled_dispatchers.clear();
  DrainState().draining_registrations.clear();
  runtime_.registry().Reset();
  return result;
}
void Controller::Initialize() { Initialize(nullptr); }
void Controller::Start() { Start(nullptr); }
bool Controller::FinalizeSnapshot() { return FinalizeSnapshot(nullptr); }
void Controller::ResetForTesting() {
  std::vector<std::shared_ptr<LoanedReceiveGate>> gates;
  std::vector<std::shared_ptr<CachedMetricsGate>> cached_gates;
  {
    std::lock_guard lock(mutex_);
    for (auto& [dispatcher, entries] : entries_) {
      DrainState().disabled_dispatchers.insert(dispatcher);
      for (auto& [_, entry] : entries.loaned_receive) {
        if (auto gate = entry.gate.lock()) gates.push_back(std::move(gate));
      }
    }
    for (const auto& [_, gate] : cached_metric_gates_) {
      gate->state.fetch_or(CachedMetricsGate::kClosed, std::memory_order_acq_rel);
      cached_gates.push_back(gate);
    }
    state_ = State::kStopped;
  }
  DisableGates(gates);
  for (const auto& gate : cached_gates) {
    std::unique_lock lock(gate->mutex);
    gate->drained.wait(lock, [&] {
      return (gate->state.load(std::memory_order_acquire) & CachedMetricsGate::kReaders) == 0;
    });
  }
  {
    std::lock_guard lock(mutex_);
    entries_.clear();
    entry_storage_.clear();
    pod_entry_storage_.clear();
    loan_drivers_.clear();
    lifecycles_.clear();
    active_instances_ = 0;
    runtime_.Initialize();
    state_ = State::kUninitialized;
    DrainState().disabled_dispatchers.clear();
    DrainState().draining_registrations.clear();
    dispatcher_drains_.clear();
    cached_entry_dispatchers_.clear();
    cached_metric_gates_.clear();
  }
}
bool Controller::Enabled() const noexcept { return runtime_.mode() == ObservationMode::kCounters; }
bool Controller::ReporterAvailable() const noexcept {
  std::lock_guard lock(mutex_);
  return Enabled() && (state_ == State::kInitialized || state_ == State::kStarted);
}
void Controller::InvalidateCachedEntries(const void* dispatcher) noexcept {
  if (!Enabled()) return;
  std::shared_ptr<CachedMetricsGate> gate;
  {
    std::lock_guard lock(mutex_);
    auto gate_it = cached_metric_gates_.find(dispatcher);
    if (gate_it != cached_metric_gates_.end()) gate = gate_it->second;
    if (gate) gate->state.fetch_or(CachedMetricsGate::kClosed, std::memory_order_acq_rel);
  }
  if (gate) {
    std::unique_lock lock(gate->mutex);
    gate->drained.wait(lock, [&] {
      return (gate->state.load(std::memory_order_acquire) & CachedMetricsGate::kReaders) == 0;
    });
  }
  std::lock_guard lock(mutex_);
  auto found = entries_.find(dispatcher);
  if (found != entries_.end())
    for (const auto& [wrapper, _] : found->second.access)
      const_cast<transport::SendTypeWrapper*>(wrapper)->cached_metrics = nullptr;
  cached_entry_dispatchers_.erase(dispatcher);
}
std::shared_ptr<CachedMetricsGate> Controller::GetCachedMetricsGate(const void* dispatcher) {
  if (!Enabled()) return nullptr;
  std::lock_guard lock(mutex_);
  if (!Enabled()) return nullptr;
  auto& gate = cached_metric_gates_[dispatcher];
  if (!gate) gate = std::make_shared<CachedMetricsGate>();
  return gate;
}
size_t Controller::StorageSizeForTesting() const {
  std::lock_guard lock(mutex_);
  return entry_storage_.size() + pod_entry_storage_.size() +
         runtime_.registry().StorageSizeForTesting();
}
bool Controller::Register(const void* dispatcher, const transport::SendTypeWrapper* wrapper,
                          std::string_view pkg, std::string_view module, std::string_view topic,
                          std::string_view type,
                          const std::vector<transport::TransportDriver*>& drivers,
                          MetricsEntry** cached_entry) {
  if (cached_entry) *cached_entry = nullptr;
  if (!Enabled()) return true;
  std::lock_guard controller_lock(mutex_);
  if (state_ != State::kInitialized || runtime_.registry().IsFrozen()) return true;
  const auto registry_mark = runtime_.registry().MarkRegistration();
  const auto entry_storage_mark = entry_storage_.size();
  std::vector<const transport::TransportDriver*> added_loan_drivers;
  auto rollback = [&] {
    for (auto* driver : added_loan_drivers) loan_drivers_.erase(driver);
    runtime_.registry().RollbackRegistration(registry_mark);
    entry_storage_.resize(entry_storage_mark);
    auto it = entries_.find(dispatcher);
    if (it != entries_.end() && it->second.access.empty() && it->second.framework.empty())
      entries_.erase(it);
  };
  bool committed = false;
  struct RegistrationGuard {
    decltype(rollback)& action;
    bool& committed;
    ~RegistrationGuard() {
      if (!committed) action();
    }
  } registration_guard{rollback, committed};
  auto values_it = entries_.find(dispatcher);
  if (values_it == entries_.end()) values_it = entries_.emplace(dispatcher, Entries{}).first;
  auto& values = values_it->second;
  const Key framework_key{std::string(pkg), std::string(module), std::string(topic),
                          std::string(type)};
  if ((wrapper && values.access.find(wrapper) != values.access.end()) ||
      values.framework.find(framework_key) != values.framework.end())
    return false;
  auto entry = std::make_unique<MetricsEntry>();
  entry->logical = runtime_.registry().RegisterLogicalSlot(topic, type, pkg, module);
  if (!entry->logical) {
    rollback();
    return true;
  }
  entry->drivers.reserve(drivers.size());
  for (auto* driver : drivers) {
    auto* slot = runtime_.registry().RegisterDriverSlot(topic, type, driver->Name(), pkg, module);
    if (!slot) {
      rollback();
      return true;
    }
    entry->drivers.push_back(slot);
    auto* loan_slot = runtime_.registry().RegisterLoanDriverSlot(driver->Name());
    if (!loan_slot) {
      rollback();
      return true;
    }
    if (loan_drivers_.emplace(driver, loan_slot).second) added_loan_drivers.push_back(driver);
  }
  if (entry->drivers.size() != drivers.size()) {
    rollback();
    return true;
  }
  values.access.reserve(values.access.size() + (wrapper ? 1 : 0));
  values.framework.reserve(values.framework.size() + 1);
  auto* entry_ptr = entry.get();
  entry_storage_.push_back(std::move(entry));
  const auto framework_result = values.framework.emplace(framework_key, entry_ptr);
  if (!framework_result.second) return false;
  if (wrapper) {
    try {
      if (!values.access.emplace(wrapper, entry_ptr).second) {
        values.framework.erase(framework_result.first);
        return false;
      }
    } catch (...) {
      values.framework.erase(framework_result.first);
      throw;
    }
  }
  if (cached_entry) *cached_entry = entry_ptr;
  if (cached_entry) cached_entry_dispatchers_.insert(dispatcher);
  committed = true;
  return true;
}
MetricsEntry* Controller::Find(const void* dispatcher,
                               const transport::SendTypeWrapper* wrapper) noexcept {
  std::lock_guard lock(mutex_);
  auto entries_it = entries_.find(dispatcher);
  if (entries_it == entries_.end()) return nullptr;
  auto found = entries_it->second.access.find(wrapper);
  return found == entries_it->second.access.end() ? nullptr : found->second;
}
MetricsEntry* Controller::Find(const void* dispatcher, std::string_view pkg,
                               std::string_view module, std::string_view topic,
                               std::string_view type) noexcept {
  std::lock_guard lock(mutex_);
  auto entries_it = entries_.find(dispatcher);
  if (entries_it == entries_.end()) return nullptr;
  auto found = entries_it->second.framework.find(
      Key{std::string(pkg), std::string(module), std::string(topic), std::string(type)});
  return found == entries_it->second.framework.end() ? nullptr : found->second;
}
Registry::DriverSlot* Controller::FindLoanDriver(
    const transport::TransportDriver* driver) noexcept {
  std::lock_guard lock(mutex_);
  auto found = loan_drivers_.find(driver);
  return found == loan_drivers_.end() ? nullptr : found->second;
}
Registry::DriverSlot* Controller::FindLoanDriver(
    const CachedMetricsGate& gate, const transport::TransportDriver* driver) noexcept {
  for (const auto& [cached_driver, slot] : gate.loan_driver_slots)
    if (cached_driver == driver) return slot;
  return FindLoanDriver(driver);
}
void Controller::CacheLoanDriverSlots(const void* dispatcher,
                                      const std::vector<transport::TransportDriver*>& drivers) {
  if (!Enabled()) return;
  std::lock_guard lock(mutex_);
  auto gate = cached_metric_gates_.find(dispatcher);
  if (gate == cached_metric_gates_.end()) return;
  auto& slots = gate->second->loan_driver_slots;
  slots.clear();
  slots.reserve(drivers.size());
  for (auto* driver : drivers) {
    auto found = loan_drivers_.find(driver);
    if (found != loan_drivers_.end()) slots.emplace_back(driver, found->second);
  }
  if (!slots.empty()) cached_entry_dispatchers_.insert(dispatcher);
}
bool Controller::RegisterPod(const void* dispatcher, const void* registration,
                             std::string_view topic, std::string_view type,
                             const transport::TransportDriver* driver,
                             PodMetricsEntry** cached_entry) {
  if (cached_entry) *cached_entry = nullptr;
  if (!Enabled()) return true;
  std::lock_guard lock(mutex_);
  if (state_ != State::kInitialized || runtime_.registry().IsFrozen()) return true;
  auto& pod = entries_[dispatcher].pod;
  if (pod.find(registration) != pod.end()) return false;
  auto out = std::make_unique<PodMetricsEntry>();
  out->logical = runtime_.registry().RegisterPodLogicalSlot(topic, type);
  out->driver = runtime_.registry().RegisterPodDriverSlot(topic, type, driver->Name());
  if (!out->logical || !out->driver) return true;
  auto* out_ptr = out.get();
  pod_entry_storage_.push_back(std::move(out));
  if (!pod.emplace(registration, out_ptr).second) return false;
  if (cached_entry) *cached_entry = out_ptr;
  if (cached_entry) cached_entry_dispatchers_.insert(dispatcher);
  return true;
}
void Controller::UnregisterPod(const void* dispatcher, const void* registration) noexcept {
  if (!Enabled()) return;
  std::lock_guard lock(mutex_);
  auto entries_it = entries_.find(dispatcher);
  if (entries_it != entries_.end()) entries_it->second.pod.erase(registration);
}
PodMetricsEntry* Controller::FindPod(const void* dispatcher, const void* registration) noexcept {
  std::lock_guard lock(mutex_);
  auto entries_it = entries_.find(dispatcher);
  if (entries_it == entries_.end()) return nullptr;
  auto found = entries_it->second.pod.find(registration);
  return found == entries_it->second.pod.end() ? nullptr : found->second;
}
std::shared_ptr<Registry::LoanedReceiveSlot> Controller::RegisterLoanedReceive(
    const void* dispatcher, const void* registration, std::string_view topic, std::string_view type,
    std::string_view driver, const std::shared_ptr<LoanedReceiveGate>& gate) {
  if (!Enabled()) return {};
  std::lock_guard lock(mutex_);
  const ReceiveRegistrationKey key{dispatcher, registration};
  if (state_ != State::kInitialized || runtime_.registry().IsFrozen() ||
      DrainState().disabled_dispatchers.find(dispatcher) !=
          DrainState().disabled_dispatchers.end() ||
      DrainState().draining_registrations.find(key) != DrainState().draining_registrations.end())
    return {};
  auto& receive = entries_[dispatcher].loaned_receive;
  if (receive.find(registration) != receive.end()) return {};
  auto slot = runtime_.registry().RegisterLoanedReceiveSlot(topic, type, driver);
  if (!slot) return {};
  if (!receive.emplace(registration, Entries::ReceiveEntry{slot, gate}).second) return {};
  return slot;
}
void Controller::UnregisterLoanedReceive(const void* dispatcher,
                                         const void* registration) noexcept {
  if (!Enabled()) return;
  std::shared_ptr<LoanedReceiveGate> gate;
  const ReceiveRegistrationKey key{dispatcher, registration};
  {
    std::lock_guard lock(mutex_);
    auto entries_it = entries_.find(dispatcher);
    if (entries_it == entries_.end()) return;
    auto found = entries_it->second.loaned_receive.find(registration);
    if (found == entries_it->second.loaned_receive.end()) return;
    gate = found->second.gate.lock();
    entries_it->second.loaned_receive.erase(found);
    DrainState().draining_registrations.insert(key);
  }
  if (gate) gate->Disable();
  {
    std::lock_guard lock(mutex_);
    DrainState().draining_registrations.erase(key);
  }
}
void Controller::DisableLoanedReceive(const void* dispatcher) noexcept {
  if (!Enabled()) return;
  std::vector<std::shared_ptr<LoanedReceiveGate>> gates;
  std::shared_ptr<DrainToken> token;
  bool owner = false;
  {
    std::lock_guard lock(mutex_);
    auto& stored = dispatcher_drains_[dispatcher];
    if (!stored) stored = std::make_shared<DrainToken>();
    token = stored;
    {
      std::lock_guard token_lock(token->mutex);
      if (!token->complete && !token->draining) {
        token->draining = true;
        token->complete = false;
        owner = true;
      }
    }
    if (owner) {
      DrainState().disabled_dispatchers.insert(dispatcher);
      auto entries_it = entries_.find(dispatcher);
      if (entries_it != entries_.end()) {
        for (auto& [_, entry] : entries_it->second.loaned_receive)
          if (auto gate = entry.gate.lock()) gates.push_back(std::move(gate));
        entries_it->second.loaned_receive.clear();
      }
      token->gates = gates;
    }
  }
  if (!owner) {
    std::vector<std::shared_ptr<LoanedReceiveGate>> shared_gates;
    {
      std::lock_guard token_lock(token->mutex);
      shared_gates = token->gates;
    }
    bool self = false;
    for (const auto& gate : shared_gates) {
      if (gate) gate->BeginDisable();
      if (gate && gate->IsCurrentInvocation()) self = true;
    }
    if (self) return;
    std::unique_lock token_lock(token->mutex);
    token->done.wait(token_lock, [&] { return token->complete; });
    token_lock.unlock();
    for (auto& gate : shared_gates)
      if (gate) gate->WaitDisabled();
    return;
  }
  DisableGates(gates);
  {
    std::lock_guard token_lock(token->mutex);
    token->draining = false;
    token->complete = true;
    token->done.notify_all();
  }
}
Controller& CoreMetricsController() { return Controller::Instance(); }
}  // namespace ibmw::runtime::core::observability
