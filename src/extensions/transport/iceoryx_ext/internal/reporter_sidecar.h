// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifdef IBMW_ENABLE_REPORTER_V1

#include "extension_api/ibmw_reporter_v1.h"

#include <array>
#include <algorithm>
#include <memory>
#include <cstddef>
#include <string_view>
#include <atomic>
#include <vector>

namespace ibmw::transport {
class Context;
}

namespace ibmw::runtime::core::transport {
struct TransportInfo;
struct LoanedSampleHandle;
}

namespace ibmw::extensions::iceoryx_extension {

class IceoryxTransportDriver;
class IoxPublisher;
class IoxSubscriptionState;
struct DirectPublisherOps;
std::unique_ptr<const DirectPublisherOps> MakeUnreportedDirectPublisherOps();

struct DirectReporterSlots {
  ibmw_reporter_slot_v1* attempt = nullptr;
  ibmw_reporter_slot_v1* success = nullptr;
  ibmw_reporter_slot_v1* fail = nullptr;
  ibmw_reporter_slot_v1* retry = nullptr;
  ibmw_reporter_slot_v1* publish = nullptr;
  ibmw_reporter_slot_v1* release = nullptr;
};

inline bool DirectReporterSlotsComplete(const DirectReporterSlots& slots) {
  return slots.attempt && slots.success && slots.fail && slots.retry &&
         slots.publish && slots.release;
}

struct IoxReceiveReporter {
  const ibmw_reporter_v1* api = nullptr;
  ibmw_reporter_slot_v1* delivered = nullptr;
  ibmw_reporter_slot_v1* callback_entered = nullptr;
  ibmw_reporter_slot_v1* callback_completed = nullptr;
  ibmw_reporter_slot_v1* callback_threw_std = nullptr;
  ibmw_reporter_slot_v1* callback_threw_nonstd = nullptr;
  ibmw_reporter_slot_v1* released = nullptr;
  ibmw_reporter_slot_v1* outstanding = nullptr;
  ibmw_reporter_slot_v1* outstanding_peak = nullptr;
  ibmw_reporter_slot_v1* limit_drop = nullptr;
};

inline bool ValidReporterBatch(const ibmw_reporter_batch_v1* api) {
  return api && api->abi_version == IBMW_REPORTER_BATCH_ABI_VERSION_V1 &&
         api->struct_size >= offsetof(ibmw_reporter_batch_v1, abort) +
                                 sizeof(api->abort) &&
         api->owner && api->prepare && api->commit && api->abort;
}

class ReporterBatchRegistration {
 public:
  ReporterBatchRegistration() = default;
  ~ReporterBatchRegistration() { Abort(); }
  ReporterBatchRegistration(const ReporterBatchRegistration&) = delete;
  ReporterBatchRegistration& operator=(const ReporterBatchRegistration&) = delete;

  template <size_t N>
  bool Prepare(const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
               std::array<ibmw_reporter_slot_v1*, N>& slots,
               const std::array<std::string_view, N>& metrics,
               std::string_view kind, std::string_view topic,
               std::string_view type) {
    if (!api || !batch || !ValidReporterBatch(batch) ||
        batch->owner != api->owner) return false;
    slots.fill(nullptr);
    std::array<ibmw_reporter_slot_descriptor_v1, N> descriptors{};
    const auto view = [](std::string_view value) {
      return ibmw_reporter_string_view_v1{
          value.data(), static_cast<uint64_t>(value.size())};
    };
    for (size_t i = 0; i < metrics.size(); ++i) {
      descriptors[i].abi_version = IBMW_REPORTER_ABI_VERSION_V1;
      descriptors[i].struct_size = sizeof(descriptors[i]);
      descriptors[i].kind = view(kind);
      descriptors[i].metric = view(metrics[i]);
      descriptors[i].topic = view(topic);
      descriptors[i].type = view(type);
      descriptors[i].driver = view("iceoryx");
    }
    batch_ = batch;
    bool prepared = false;
    try {
      prepared = batch->prepare(batch->owner, descriptors.data(), metrics.size(),
                                slots.data(), &transaction_) != 0;
    } catch (...) {
      prepared = false;
    }
    const bool complete = prepared && transaction_ &&
        std::all_of(slots.begin(), slots.end(), [](const auto* slot) {
          return slot != nullptr;
        });
    if (!complete) {
      Abort();
      slots.fill(nullptr);
      return false;
    }
    return true;
  }

  bool Commit() noexcept {
    if (!batch_ || !transaction_) return false;
    bool committed = false;
    try {
      committed = batch_->commit(batch_->owner, transaction_) != 0;
    } catch (...) {
      committed = false;
    }
    if (committed) { batch_ = nullptr; transaction_ = nullptr; }
    return committed;
  }
  bool Active() const noexcept { return transaction_ != nullptr; }
  void Abort() noexcept {
    if (batch_ && transaction_) {
      try {
        batch_->abort(batch_->owner, transaction_);
      } catch (...) {
      }
    }
    batch_ = nullptr;
    transaction_ = nullptr;
  }

 private:
  const ibmw_reporter_batch_v1* batch_ = nullptr;
  ibmw_reporter_batch_transaction_v1* transaction_ = nullptr;
};

inline bool PrepareIoxReceiveReporterSlots(
    const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
    ReporterBatchRegistration& transaction, IoxReceiveReporter& reporter,
    std::string_view topic, std::string_view type) {
  reporter.api = api;
  std::array<ibmw_reporter_slot_v1*, 9> slots{};
  if (!transaction.Prepare(api, batch, slots,
                             {"receive_delivered", "callback_entered",
                              "callback_completed", "callback_threw_std",
                              "callback_threw_nonstd", "receive_released",
                              "outstanding_current", "outstanding_peak",
                              "limit_drop"},
                             "retained_iceoryx_receive", topic, type))
    return false;
  reporter.delivered = slots[0]; reporter.callback_entered = slots[1];
  reporter.callback_completed = slots[2]; reporter.callback_threw_std = slots[3];
  reporter.callback_threw_nonstd = slots[4]; reporter.released = slots[5];
  reporter.outstanding = slots[6]; reporter.outstanding_peak = slots[7];
  reporter.limit_drop = slots[8];
  return true;
}

inline bool RegisterIoxReceiveReporterSlots(
    const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
    IoxReceiveReporter& reporter, std::string_view topic,
    std::string_view type) {
  ReporterBatchRegistration transaction;
  return PrepareIoxReceiveReporterSlots(api, batch, transaction, reporter,
                                        topic, type) && transaction.Commit();
}

struct DirectReporterBinding {
  const ibmw_reporter_v1* api = nullptr;
  DirectReporterSlots slots;
  IoxReceiveReporter receive;
};

struct DirectPublisherOps {
  using RawLoan = bool (*)(
      const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
      const runtime::core::transport::TransportInfo&, size_t,
      const ibmw::transport::Context&,
      runtime::core::transport::LoanedSampleHandle&);
  using SerializedLoan = bool (*)(
      const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
      const runtime::core::transport::TransportInfo&,
      const ibmw::transport::Context&, const void*,
      runtime::core::transport::LoanedSampleHandle&);
  using Complete = void (*)(const DirectPublisherOps*, IoxPublisher*,
                             void*) noexcept;

  RawLoan raw_loan;
  SerializedLoan serialized_loan;
  Complete publish;
  Complete release;
  DirectReporterBinding binding;

  bool Reported() const noexcept { return binding.api != nullptr; }

  template <bool Reported>
  static bool RawLoanImpl(
      const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
      const runtime::core::transport::TransportInfo&, size_t,
      const ibmw::transport::Context&,
      runtime::core::transport::LoanedSampleHandle&);
  template <bool Reported>
  static bool LoanImpl(
      const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
      const runtime::core::transport::TransportInfo&,
      const ibmw::transport::Context&, const void*,
      runtime::core::transport::LoanedSampleHandle&);
  template <bool Reported, bool Publish>
  static void CompleteImpl(const DirectPublisherOps*, IoxPublisher*,
                           void*) noexcept;
};

struct PublisherReporterState {
  std::atomic<const DirectPublisherOps*> active{nullptr};
  std::vector<std::unique_ptr<const DirectPublisherOps>> storage;
};

struct SubscriptionReporterState {
  std::unique_ptr<const IoxReceiveReporter> reporter;
  std::shared_ptr<ReporterBatchRegistration> transaction;
  size_t reported_peak = 0;
};

struct DriverReporterState {
  const ibmw_reporter_v1* api = nullptr;
  const ibmw_reporter_batch_v1* batch = nullptr;
};

class ReporterSidecarAccess {
 public:
  static PublisherReporterState* Publisher(IoxPublisher* publisher) noexcept;
  static const PublisherReporterState* Publisher(
      const IoxPublisher* publisher) noexcept;
  static SubscriptionReporterState* Subscription(
      IoxSubscriptionState* subscription) noexcept;
  static const DirectPublisherOps* DirectOps(
      const IoxPublisher* publisher) noexcept;
  static bool PreparePublisherInstall(IoxPublisher* publisher) noexcept;
  static void InstallPublisher(
      IoxPublisher* publisher,
      std::unique_ptr<const DirectPublisherOps> direct_ops) noexcept;
};

template <bool Reported>
class DirectLoanReporterOutcome {
 public:
  explicit DirectLoanReporterOutcome(DirectReporterBinding*) noexcept {}
  void MarkSuccess() noexcept {}
};

template <>
class DirectLoanReporterOutcome<true> {
 public:
  explicit DirectLoanReporterOutcome(DirectReporterBinding* binding) noexcept
      : binding_(binding) {
    Add(binding_->slots.attempt);
  }
  ~DirectLoanReporterOutcome() {
    if (!success_) Add(binding_->slots.fail);
  }
  void MarkSuccess() noexcept {
    Add(binding_->slots.success);
    success_ = true;
  }

 private:
  void Add(ibmw_reporter_slot_v1* slot) noexcept {
    if (binding_->api->add && slot) {
      binding_->api->add(binding_->api->owner, slot, 1);
    }
  }

  DirectReporterBinding* binding_;
  bool success_ = false;
};

}  // namespace ibmw::extensions::iceoryx_extension

#endif
