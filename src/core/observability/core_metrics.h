#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef IBMW_ENABLE_REPORTER_V1
#include "extension_api/ibmw_reporter_v1.h"
#endif

namespace ibmw::runtime::core::transport { class SendTypeWrapper; class TransportDriver; }

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
  struct LogicalSlot { LogicalSlot(std::string p,std::string m,std::string t,std::string y):pkg(std::move(p)),module(std::move(m)),topic(std::move(t)),type(std::move(y)){} std::string pkg,module,topic,type; std::atomic<uint64_t> attempted{0},rejected{0},loan_attempted{0},loan_succeeded{0},loan_failed{0}; };
  struct DriverSlot { DriverSlot(std::string p,std::string m,std::string t,std::string y,std::string d):pkg(std::move(p)),module(std::move(m)),topic(std::move(t)),type(std::move(y)),driver(std::move(d)){} std::string pkg,module,topic,type,driver; std::atomic<uint64_t> attempted{0},returned{0},loan_publish_called{0},loan_release_called{0}; };
  struct PodLogicalSlot { PodLogicalSlot(std::string t,std::string y):topic(std::move(t)),type(std::move(y)){} std::string topic,type; std::atomic<uint64_t> attempted{0},succeeded{0},failed{0}; };
  struct PodDriverSlot { PodDriverSlot(std::string t,std::string y,std::string d):topic(std::move(t)),type(std::move(y)),driver(std::move(d)){} std::string topic,type,driver; std::atomic<uint64_t> publish_called{0},release_called{0}; };
  // Loaned callback outcomes. Retained outstanding/limit accounting remains
  // transport-specific; ordinary OnMessage is intentionally out of scope.
  struct LoanedReceiveSlot { LoanedReceiveSlot(std::string t,std::string y,std::string d):topic(std::move(t)),type(std::move(y)),driver(std::move(d)){} std::string topic,type,driver; std::atomic<uint64_t> callback_entered{0},callback_completed{0},callback_threw_std{0},callback_threw_nonstd{0},release_called{0}; };
#ifdef IBMW_ENABLE_REPORTER_V1
  struct ReporterSlot { std::string kind,metric,topic,type,driver; std::atomic<uint64_t> value{0}; };
  ReporterSlot* RegisterReporterSlot(const ibmw_reporter_slot_descriptor_v1* descriptor);
#endif
  bool RegisterLogical(std::string_view, std::string_view);
  bool RegisterDriver(std::string_view, std::string_view, std::string_view);
  DriverSlot* RegisterLoanDriverSlot(std::string_view driver);
  PodLogicalSlot* RegisterPodLogicalSlot(std::string_view topic, std::string_view type);
  PodDriverSlot* RegisterPodDriverSlot(std::string_view topic, std::string_view type, std::string_view driver);
  std::shared_ptr<LoanedReceiveSlot> RegisterLoanedReceiveSlot(std::string_view topic, std::string_view type, std::string_view driver);
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
  LogicalSlot* RegisterLogicalSlot(std::string_view topic, std::string_view type, std::string_view pkg = {}, std::string_view module = {});
  DriverSlot* RegisterDriverSlot(std::string_view topic, std::string_view type, std::string_view driver, std::string_view pkg = {}, std::string_view module = {});
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
#ifdef IBMW_ENABLE_REPORTER_V1
  std::vector<ReporterSlot*> reporter_slots_;
  std::vector<std::unique_ptr<ReporterSlot>> reporter_storage_;
#endif
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
  explicit Runtime(ObservationMode mode=ObservationMode::kOff):mode_(mode){}
  ObservationMode mode() const noexcept{return mode_.load(std::memory_order_acquire);}
  Registry& registry() noexcept{return registry_;}
  const Registry& registry() const noexcept{return registry_;}
  void Initialize();
  void Start(){registry_.Freeze();}
  std::string FinalSnapshotJsonl() const{return registry_.SnapshotJsonl();}
  bool FinalizeSnapshot();
 private: std::atomic<ObservationMode> mode_; Registry registry_;
};

struct MetricsEntry { Registry::LogicalSlot* logical = nullptr; std::vector<Registry::DriverSlot*> drivers; };
struct PodMetricsEntry { Registry::PodLogicalSlot* logical = nullptr; Registry::PodDriverSlot* driver = nullptr; };
class Controller {
 public:
  static Controller& Instance();
  void Initialize(const void* identity); void Start(const void* identity);
  bool FinalizeSnapshot(const void* identity);
  void Initialize(); void Start(); bool FinalizeSnapshot(); bool Enabled() const noexcept;
  bool ReporterAvailable() const noexcept;
  // Process-global controller isolation hook for unit tests. Production
  // lifecycle code must use Initialize/Start/FinalizeSnapshot instead.
  void ResetForTesting();
  size_t StorageSizeForTesting() const;
  bool Register(const void*, const transport::SendTypeWrapper*, std::string_view pkg, std::string_view module, std::string_view topic, std::string_view type, const std::vector<transport::TransportDriver*>&);
  MetricsEntry* Find(const void*, const transport::SendTypeWrapper*) noexcept;
  MetricsEntry* Find(const void*, std::string_view pkg, std::string_view module, std::string_view topic, std::string_view type) noexcept;
  Registry::DriverSlot* FindLoanDriver(const transport::TransportDriver*) noexcept;
  bool RegisterPod(const void* dispatcher, const void* registration,
                   std::string_view topic, std::string_view type,
                   const transport::TransportDriver*);
#ifdef IBMW_ENABLE_REPORTER_V1
  Registry::ReporterSlot* RegisterReporterSlot(
      const ibmw_reporter_slot_descriptor_v1* descriptor) noexcept;
#endif
  void UnregisterPod(const void* dispatcher, const void* registration) noexcept;
  PodMetricsEntry* FindPod(const void* dispatcher,
                           const void* registration) noexcept;
  std::shared_ptr<Registry::LoanedReceiveSlot> RegisterLoanedReceive(
      const void* dispatcher, const void* registration, std::string_view topic,
      std::string_view type, std::string_view driver,
      const std::shared_ptr<LoanedReceiveGate>& lifecycle_gate);
  void UnregisterLoanedReceive(const void* dispatcher,
                               const void* registration) noexcept;
  void DisableLoanedReceive(const void* dispatcher) noexcept;
  Registry& registry() noexcept { return runtime_.registry(); }
 private:
  enum class State { kUninitialized, kInitialized, kStarted, kStopped };
  struct Key { std::string pkg, module, topic, type; bool operator==(const Key& x) const noexcept { return pkg==x.pkg && module==x.module && topic==x.topic && type==x.type; } };
  struct Hash { size_t operator()(const Key& x) const noexcept { return std::hash<std::string>{}(x.pkg) ^ (std::hash<std::string>{}(x.module)<<1) ^ (std::hash<std::string>{}(x.topic)<<2) ^ (std::hash<std::string>{}(x.type)<<3); } };
  struct Entries {
    std::unordered_map<const transport::SendTypeWrapper*, MetricsEntry*> access;
    std::unordered_map<Key, MetricsEntry*, Hash> framework;
    std::unordered_map<const void*, PodMetricsEntry*> pod;
    struct ReceiveEntry { std::shared_ptr<Registry::LoanedReceiveSlot> slot; std::weak_ptr<LoanedReceiveGate> gate; };
    std::unordered_map<const void*, ReceiveEntry> loaned_receive;
  };
  struct Lifecycle { bool initialized = false; bool started = false; };
  struct DrainToken {
    std::mutex mutex;
    std::condition_variable done;
    bool draining = false;
    bool complete = false;
    std::vector<std::shared_ptr<LoanedReceiveGate>> gates;
  };
  mutable std::mutex mutex_;
  Runtime runtime_; std::unordered_map<const void*, Entries> entries_;
  std::vector<std::unique_ptr<MetricsEntry>> entry_storage_;
  std::vector<std::unique_ptr<PodMetricsEntry>> pod_entry_storage_;
  std::unordered_map<const void*, std::shared_ptr<DrainToken>> dispatcher_drains_;
  std::unordered_map<const transport::TransportDriver*, Registry::DriverSlot*> loan_drivers_;
  std::unordered_map<const void*, Lifecycle> lifecycles_;
  State state_ = State::kUninitialized;
  size_t active_instances_ = 0;
};
Controller& CoreMetricsController();
}
