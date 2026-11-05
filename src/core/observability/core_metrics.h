// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "extension_api/ibmw_reporter_v1.h"

namespace ibmw::runtime::core::observability {
struct MetricsEntry;
}
namespace ibmw::runtime::core::transport {
struct SendTypeWrapper;
class TransportDriver;
}  // namespace ibmw::runtime::core::transport

namespace ibmw::runtime::core::observability {
enum class ObservationMode { kOff, kCounters };
class LoanedReceiveGate {
 public:
  virtual ~LoanedReceiveGate() = default;
  virtual void Disable() noexcept = 0;
  virtual void BeginDisable() noexcept { Disable(); }
  virtual void WaitDisabled() noexcept {}
  virtual bool IsCurrentInvocation() const noexcept { return false; }
};
class Registry {
 public:
  Registry();
  ~Registry();
  struct LogicalSlot {
    LogicalSlot(std::string pkg, std::string module, std::string topic, std::string type)
        : pkg(std::move(pkg)),
          module(std::move(module)),
          topic(std::move(topic)),
          type(std::move(type)) {}
    std::string pkg, module, topic, type;
    std::atomic<uint64_t> attempted{0}, rejected{0}, loan_attempted{0}, loan_succeeded{0},
        loan_failed{0};
  };
  struct DriverSlot {
    DriverSlot(std::string pkg, std::string module, std::string topic, std::string type,
               std::string driver)
        : pkg(std::move(pkg)),
          module(std::move(module)),
          topic(std::move(topic)),
          type(std::move(type)),
          driver(std::move(driver)) {}
    std::string pkg, module, topic, type, driver;
    std::atomic<uint64_t> attempted{0}, returned{0}, loan_publish_called{0}, loan_release_called{0};
  };
  struct PodLogicalSlot {
    PodLogicalSlot(std::string topic, std::string type)
        : topic(std::move(topic)), type(std::move(type)) {}
    std::string topic, type;
    std::atomic<uint64_t> attempted{0}, succeeded{0}, failed{0};
  };
  struct PodDriverSlot {
    PodDriverSlot(std::string topic, std::string type, std::string driver)
        : topic(std::move(topic)), type(std::move(type)), driver(std::move(driver)) {}
    std::string topic, type, driver;
    std::atomic<uint64_t> publish_called{0}, release_called{0};
  };
  // Loaned callback outcomes. Retained outstanding/limit accounting remains
  // transport-specific; ordinary OnMessage is intentionally out of scope.
  struct LoanedReceiveSlot {
    LoanedReceiveSlot(std::string topic, std::string type, std::string driver)
        : topic(std::move(topic)), type(std::move(type)), driver(std::move(driver)) {}
    std::string topic, type, driver;
    std::atomic<uint64_t> callback_entered{0}, callback_completed{0}, callback_threw_std{0},
        callback_threw_nonstd{0}, release_called{0};
  };
  struct ReporterSlot;
  ReporterSlot* RegisterReporterSlot(const ibmw_reporter_slot_descriptor_v1* descriptor);
  bool PrepareReporterBatch(const ibmw_reporter_slot_descriptor_v1* descriptors, size_t count,
                            ibmw_reporter_slot_v1** slots,
                            ibmw_reporter_batch_transaction_v1** transaction);
  bool CommitReporterBatch(ibmw_reporter_batch_transaction_v1* transaction);
  void AbortReporterBatch(ibmw_reporter_batch_transaction_v1* transaction) noexcept;
  bool HasPendingReporterBatches() const noexcept;
  uint64_t ReporterValueForTesting(const ReporterSlot* slot) const noexcept;
  bool RegisterLogical(std::string_view, std::string_view);
  bool RegisterDriver(std::string_view, std::string_view, std::string_view);
  DriverSlot* RegisterLoanDriverSlot(std::string_view driver);
  struct RegistrationMark {
    size_t logical, driver;
  };
  RegistrationMark MarkRegistration() const;
  void RollbackRegistration(RegistrationMark mark) noexcept;
  PodLogicalSlot* RegisterPodLogicalSlot(std::string_view topic, std::string_view type);
  PodDriverSlot* RegisterPodDriverSlot(std::string_view topic, std::string_view type,
                                       std::string_view driver);
  std::shared_ptr<LoanedReceiveSlot> RegisterLoanedReceiveSlot(std::string_view topic,
                                                               std::string_view type,
                                                               std::string_view driver);
  void Freeze();
  void Reset();
  bool IsFrozen() const noexcept { return frozen_.load(std::memory_order_acquire); }
  void AddLogicalAttempt(std::string_view, std::string_view);
  void AddLogicalRejected(std::string_view, std::string_view);
  void AddSendAttempt(std::string_view, std::string_view, std::string_view);
  void AddSendReturned(std::string_view, std::string_view, std::string_view);
  std::string SnapshotJsonl() const;
  std::string ReporterSnapshotJsonl(uint64_t first_sequence = 1) const;
  size_t StorageSizeForTesting() const;
  LogicalSlot* RegisterLogicalSlot(std::string_view topic, std::string_view type,
                                   std::string_view pkg = {}, std::string_view module = {});
  DriverSlot* RegisterDriverSlot(std::string_view topic, std::string_view type,
                                 std::string_view driver, std::string_view pkg = {},
                                 std::string_view module = {});
  void AddLogicalAttempt(LogicalSlot*) noexcept;
  void AddLogicalRejected(LogicalSlot*) noexcept;
  void AddSendAttempt(DriverSlot*) noexcept;
  void AddSendReturned(DriverSlot*) noexcept;
  void AddLoanAttempt(LogicalSlot*) noexcept;
  void AddLoanSucceeded(LogicalSlot*) noexcept;
  void AddLoanFailed(LogicalSlot*) noexcept;
  // Count dispatcher invocations only; TransportDriver's void API cannot
  // report whether publishing/releasing the underlying transport succeeded.
  void AddLoanPublishCalled(DriverSlot*) noexcept;
  void AddLoanReleaseCalled(DriverSlot*) noexcept;
  void AddPodLoanAttempt(PodLogicalSlot*) noexcept;
  void AddPodLoanSucceeded(PodLogicalSlot*) noexcept;
  void AddPodLoanFailed(PodLogicalSlot*) noexcept;
  void AddPodLoanPublishCalled(PodDriverSlot*) noexcept;
  void AddPodLoanReleaseCalled(PodDriverSlot*) noexcept;
  bool WriteSnapshot(const std::string& directory) const;

 private:
  std::string SerializeJsonl(bool include_core, bool include_reporter,
                             uint64_t first_sequence) const;
  mutable std::mutex mutex_;
  std::vector<LogicalSlot*> logical_slots_;
  std::vector<DriverSlot*> driver_slots_;
  std::vector<PodLogicalSlot*> pod_logical_slots_;
  std::vector<PodDriverSlot*> pod_driver_slots_;
  std::vector<std::unique_ptr<LogicalSlot>> logical_storage_;
  std::vector<std::unique_ptr<DriverSlot>> driver_storage_;
  std::vector<std::unique_ptr<PodLogicalSlot>> pod_logical_storage_;
  std::vector<std::unique_ptr<PodDriverSlot>> pod_driver_storage_;
  std::vector<std::shared_ptr<LoanedReceiveSlot>> loaned_receive_slots_;
  struct ReporterState;
  ReporterState* reporter_state_ = nullptr;
  void ResetReporterLocked();
  void DestroyReporter() noexcept;
  void AppendReporterJsonlLocked(std::ostringstream&, uint64_t&) const;
  size_t ReporterStorageSizeLocked() const noexcept;
  std::unordered_map<std::string, LogicalSlot*> logical_;
  std::unordered_map<std::string, DriverSlot*> drivers_;
  std::unordered_map<std::string, DriverSlot*> loan_drivers_;
  std::unordered_map<std::string, PodLogicalSlot*> pod_logical_;
  std::unordered_map<std::string, PodDriverSlot*> pod_drivers_;
  std::atomic_bool frozen_{false};
};
class Runtime {
 public:
  static ObservationMode ModeFromEnvironment();
  explicit Runtime(ObservationMode mode = ObservationMode::kOff) : mode_(mode) {}
  ObservationMode mode() const noexcept { return mode_.load(std::memory_order_acquire); }
  Registry& registry() noexcept { return registry_; }
  const Registry& registry() const noexcept { return registry_; }
  void Initialize();
  void Start() { registry_.Freeze(); }
  std::string FinalSnapshotJsonl() const { return registry_.SnapshotJsonl(); }
  bool FinalizeSnapshot();

 private:
  std::atomic<ObservationMode> mode_;
  Registry registry_;
};

struct MetricsEntry {
  Registry::LogicalSlot* logical = nullptr;
  std::vector<Registry::DriverSlot*> drivers;
};
struct PodMetricsEntry {
  Registry::PodLogicalSlot* logical = nullptr;
  Registry::PodDriverSlot* driver = nullptr;
};
struct CachedMetricsGate {
  static constexpr size_t kClosed = size_t{1} << (sizeof(size_t) * 8 - 1);
  static constexpr size_t kReaders = kClosed - 1;
  std::atomic_size_t state{0};
  std::mutex mutex;
  std::condition_variable drained;
  // Written before the owning dispatcher enters kStart, then read only under an
  // admitted CachedMetricsReadGuard.
  std::vector<std::pair<const transport::TransportDriver*, Registry::DriverSlot*>>
      loan_driver_slots;
};
class CachedMetricsReadGuard {
 public:
  explicit CachedMetricsReadGuard(std::shared_ptr<CachedMetricsGate> gate) noexcept
      : gate_(std::move(gate)) {
    if (!gate_) return;
    auto state = gate_->state.load(std::memory_order_relaxed);
    while ((state & CachedMetricsGate::kClosed) == 0) {
      if (gate_->state.compare_exchange_weak(state, state + 1, std::memory_order_acquire,
                                             std::memory_order_relaxed)) {
        admitted_ = true;
        break;
      }
    }
  }
  CachedMetricsReadGuard(const CachedMetricsReadGuard&) = delete;
  CachedMetricsReadGuard& operator=(const CachedMetricsReadGuard&) = delete;
  CachedMetricsReadGuard(CachedMetricsReadGuard&& other) noexcept
      : gate_(std::move(other.gate_)), admitted_(other.admitted_) {
    other.admitted_ = false;
  }
  ~CachedMetricsReadGuard() { Release(); }
  explicit operator bool() const noexcept { return admitted_; }

 private:
  void Release() noexcept {
    if (!admitted_) return;
    admitted_ = false;
    if ((gate_->state.fetch_sub(1, std::memory_order_release) & CachedMetricsGate::kReaders) == 1) {
      std::lock_guard lock(gate_->mutex);
      gate_->drained.notify_all();
    }
  }
  std::shared_ptr<CachedMetricsGate> gate_;
  bool admitted_ = false;
};
class Controller {
 public:
  static Controller& Instance();
  void Initialize(const void* identity);
  void Start(const void* identity);
  bool FinalizeSnapshot(const void* identity);
  void Initialize();
  void Start();
  bool FinalizeSnapshot();
  bool Enabled() const noexcept;
  bool ReporterAvailable() const noexcept;
  // Invalidate dispatcher-owned cached MetricsEntry pointers after quiescence.
  void InvalidateCachedEntries(const void* dispatcher) noexcept;
  std::shared_ptr<CachedMetricsGate> GetCachedMetricsGate(const void* dispatcher);
  // Process-global controller isolation hook for unit tests. Production
  // lifecycle code must use Initialize/Start/FinalizeSnapshot instead.
  void ResetForTesting();
  size_t StorageSizeForTesting() const;
  bool Register(const void*, const transport::SendTypeWrapper*, std::string_view pkg,
                std::string_view module, std::string_view topic, std::string_view type,
                const std::vector<transport::TransportDriver*>&,
                MetricsEntry** cached_entry = nullptr);
  MetricsEntry* Find(const void*, const transport::SendTypeWrapper*) noexcept;
  MetricsEntry* Find(const void*, std::string_view pkg, std::string_view module,
                     std::string_view topic, std::string_view type) noexcept;
  Registry::DriverSlot* FindLoanDriver(const transport::TransportDriver*) noexcept;
  // Lock-free for drivers cached by CacheLoanDriverSlots; others take the locked lookup.
  Registry::DriverSlot* FindLoanDriver(const CachedMetricsGate&,
                                       const transport::TransportDriver*) noexcept;
  void CacheLoanDriverSlots(const void* dispatcher,
                            const std::vector<transport::TransportDriver*>&);
  bool RegisterPod(const void* dispatcher, const void* registration, std::string_view topic,
                   std::string_view type, const transport::TransportDriver*,
                   PodMetricsEntry** cached_entry = nullptr);
  Registry::ReporterSlot* RegisterReporterSlot(
      const ibmw_reporter_slot_descriptor_v1* descriptor) noexcept;
  bool PrepareReporterBatch(const ibmw_reporter_slot_descriptor_v1* descriptors, size_t count,
                            ibmw_reporter_slot_v1** slots,
                            ibmw_reporter_batch_transaction_v1** transaction) noexcept;
  bool CommitReporterBatch(ibmw_reporter_batch_transaction_v1* transaction) noexcept;
  void AbortReporterBatch(ibmw_reporter_batch_transaction_v1* transaction) noexcept;
  void UnregisterPod(const void* dispatcher, const void* registration) noexcept;
  PodMetricsEntry* FindPod(const void* dispatcher, const void* registration) noexcept;
  std::shared_ptr<Registry::LoanedReceiveSlot> RegisterLoanedReceive(
      const void* dispatcher, const void* registration, std::string_view topic,
      std::string_view type, std::string_view driver,
      const std::shared_ptr<LoanedReceiveGate>& lifecycle_gate);
  void UnregisterLoanedReceive(const void* dispatcher, const void* registration) noexcept;
  void DisableLoanedReceive(const void* dispatcher) noexcept;
  Registry& registry() noexcept { return runtime_.registry(); }

 private:
  enum class State { kUninitialized, kInitialized, kStarted, kStopped };
  struct Key {
    std::string pkg, module, topic, type;
    bool operator==(const Key& other) const noexcept {
      return pkg == other.pkg && module == other.module && topic == other.topic &&
             type == other.type;
    }
  };
  struct Hash {
    size_t operator()(const Key& key) const noexcept {
      return std::hash<std::string>{}(key.pkg) ^ (std::hash<std::string>{}(key.module) << 1) ^
             (std::hash<std::string>{}(key.topic) << 2) ^ (std::hash<std::string>{}(key.type) << 3);
    }
  };
  struct Entries {
    std::unordered_map<const transport::SendTypeWrapper*, MetricsEntry*> access;
    std::unordered_map<Key, MetricsEntry*, Hash> framework;
    std::unordered_map<const void*, PodMetricsEntry*> pod;
    struct ReceiveEntry {
      std::shared_ptr<Registry::LoanedReceiveSlot> slot;
      std::weak_ptr<LoanedReceiveGate> gate;
    };
    std::unordered_map<const void*, ReceiveEntry> loaned_receive;
  };
  struct Lifecycle {
    bool initialized = false;
    bool started = false;
  };
  struct DrainToken {
    std::mutex mutex;
    std::condition_variable done;
    bool draining = false;
    bool complete = false;
    std::vector<std::shared_ptr<LoanedReceiveGate>> gates;
  };
  mutable std::mutex mutex_;
  Runtime runtime_;
  std::unordered_map<const void*, Entries> entries_;
  std::vector<std::unique_ptr<MetricsEntry>> entry_storage_;
  std::vector<std::unique_ptr<PodMetricsEntry>> pod_entry_storage_;
  std::unordered_map<const void*, std::shared_ptr<DrainToken>> dispatcher_drains_;
  std::unordered_set<const void*> cached_entry_dispatchers_;
  std::unordered_map<const void*, std::shared_ptr<CachedMetricsGate>> cached_metric_gates_;
  std::unordered_map<const transport::TransportDriver*, Registry::DriverSlot*> loan_drivers_;
  std::unordered_map<const void*, Lifecycle> lifecycles_;
  State state_ = State::kUninitialized;
  size_t active_instances_ = 0;
};
Controller& CoreMetricsController();
}  // namespace ibmw::runtime::core::observability
