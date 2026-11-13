// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifdef IBMW_ENABLE_REPORTER_V1

// Reporter V1 sidecar of the shm transport: the counterpart of
// iceoryx_ext/internal/reporter_sidecar.h. Two slot families, registered as
// one batch each, with the same metric names as iceoryx and the configured
// driver name ("shm", or "iceoryx" when shm replaces it in place) as label.
//
// kind direct_shm_loan (per std publisher, created by RegisterSendType):
// counted for ctx-meta loans only (LoanSample(kCtxMetaThenPayload) and
// LoanSerializedSample); POD loans and framed Send() are not counted, the
// same as iceoryx.
//   logical_attempt  every loan call that found the publisher
//   logical_success  the call returned a handle
//   logical_fail     the call returned false or threw
//   internal_retry   LoanSerializedSample took a bigger chunk after the
//                    serializer ran out of space (not on the last attempt)
//   publish          the handle's chunk was published (TryPublishLoaned)
//   release          the handle's chunk was given back: ReleaseLoaned, or a
//                    publish the library refused (out of memory)
// A stale or duplicate handle owns no chunk and counts neither, so at the end
// logical_attempt == success + fail and logical_success == publish + release.
// The second holds for handles completed before Shutdown: Shutdown reclaims the
// chunk of a handle still on loan, which then counts neither (as in iceoryx).
//
// kind retained_shm_receive (per AttachLoanedRetained subscription):
//   receive_delivered      a sample took a lease (outstanding went up)
//   callback_entered       the retained callback was entered
//   callback_completed     it returned
//   callback_threw_std     it threw a std::exception
//   callback_threw_nonstd  it threw anything else
//   receive_released       a lease was given back (outstanding went down)
//   outstanding_current    leases held now (signed +1 / -1)
//   outstanding_peak       highest outstanding_current seen
//   limit_drop             samples dropped at the max_outstanding limit: on
//                          arrival, and the ones queued behind a full lease
//                          table that the next release drains
// A sample whose release token cannot be allocated is delivered and released
// without a callback. Queued samples discarded by Stop()/shutdown were never
// leased and are not counted, as on iceoryx. Every iceoryx metric has an shm
// counterpart; none is left out.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "extension_api/ibmw_reporter_v1.h"

namespace ibmw::extensions::shm_extension {

struct DirectReporterSlots {
  ibmw_reporter_slot_v1* attempt = nullptr;
  ibmw_reporter_slot_v1* success = nullptr;
  ibmw_reporter_slot_v1* fail = nullptr;
  ibmw_reporter_slot_v1* retry = nullptr;
  ibmw_reporter_slot_v1* publish = nullptr;
  ibmw_reporter_slot_v1* release = nullptr;
};

inline bool DirectReporterSlotsComplete(const DirectReporterSlots& slots) {
  return slots.attempt && slots.success && slots.fail && slots.retry && slots.publish && slots.release;
}

// Installed on a ShmPublisher (ShmPublisher::Reporter()).
struct DirectLoanReporter {
  const ibmw_reporter_v1* api = nullptr;
  DirectReporterSlots slots;
};

struct ShmReceiveReporter {
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

inline void ReporterAdd(const ibmw_reporter_v1* api, ibmw_reporter_slot_v1* slot, uint64_t value = 1) noexcept {
  api->add(api->owner, slot, value);
}

inline bool ValidReporterBatch(const ibmw_reporter_batch_v1* api) {
  return api && api->abi_version == IBMW_REPORTER_BATCH_ABI_VERSION_V1 &&
         api->struct_size >= offsetof(ibmw_reporter_batch_v1, abort) + sizeof(api->abort) && api->owner &&
         api->prepare && api->commit && api->abort;
}

// One batch of slots: prepared all or nothing, aborted unless committed.
class ReporterBatchRegistration {
 public:
  ReporterBatchRegistration() = default;
  ~ReporterBatchRegistration() { Abort(); }
  ReporterBatchRegistration(const ReporterBatchRegistration&) = delete;
  ReporterBatchRegistration& operator=(const ReporterBatchRegistration&) = delete;

  template <size_t N>
  bool Prepare(const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
               std::array<ibmw_reporter_slot_v1*, N>& slots, const std::array<std::string_view, N>& metrics,
               std::string_view kind, std::string_view topic, std::string_view type, std::string_view driver) {
    if (!api || !batch || !ValidReporterBatch(batch) || batch->owner != api->owner) return false;
    slots.fill(nullptr);
    std::array<ibmw_reporter_slot_descriptor_v1, N> descriptors{};
    const auto view = [](std::string_view value) {
      return ibmw_reporter_string_view_v1{value.data(), static_cast<uint64_t>(value.size())};
    };
    for (size_t i = 0; i < metrics.size(); ++i) {
      descriptors[i].abi_version = IBMW_REPORTER_ABI_VERSION_V1;
      descriptors[i].struct_size = sizeof(descriptors[i]);
      descriptors[i].kind = view(kind);
      descriptors[i].metric = view(metrics[i]);
      descriptors[i].topic = view(topic);
      descriptors[i].type = view(type);
      descriptors[i].driver = view(driver);
    }
    batch_ = batch;
    bool prepared = false;
    try {
      prepared = batch->prepare(batch->owner, descriptors.data(), metrics.size(), slots.data(), &transaction_) != 0;
    } catch (...) {
      prepared = false;
    }
    const bool complete = prepared && transaction_ &&
                          std::all_of(slots.begin(), slots.end(), [](const auto* slot) { return slot != nullptr; });
    if (!complete) {
      Abort();
      slots.fill(nullptr);
      return false;
    }
    return true;
  }

  // A failed commit stays prepared and may be retried.
  bool Commit() noexcept {
    if (!batch_ || !transaction_) return false;
    bool committed = false;
    try {
      committed = batch_->commit(batch_->owner, transaction_) != 0;
    } catch (...) {
      committed = false;
    }
    if (committed) {
      batch_ = nullptr;
      transaction_ = nullptr;
    }
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

inline bool PrepareDirectLoanReporterSlots(const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
                                           ReporterBatchRegistration& transaction, DirectLoanReporter& reporter,
                                           std::string_view topic, std::string_view type, std::string_view driver) {
  reporter.api = api;
  std::array<ibmw_reporter_slot_v1*, 6> slots{};
  if (!transaction.Prepare(api, batch, slots,
                           {"logical_attempt", "logical_success", "logical_fail", "internal_retry", "publish",
                            "release"},
                           "direct_shm_loan", topic, type, driver)) {
    return false;
  }
  reporter.slots = {slots[0], slots[1], slots[2], slots[3], slots[4], slots[5]};
  return true;
}

inline bool PrepareShmReceiveReporterSlots(const ibmw_reporter_v1* api, const ibmw_reporter_batch_v1* batch,
                                           ReporterBatchRegistration& transaction, ShmReceiveReporter& reporter,
                                           std::string_view topic, std::string_view type, std::string_view driver) {
  reporter.api = api;
  std::array<ibmw_reporter_slot_v1*, 9> slots{};
  if (!transaction.Prepare(api, batch, slots,
                           {"receive_delivered", "callback_entered", "callback_completed", "callback_threw_std",
                            "callback_threw_nonstd", "receive_released", "outstanding_current", "outstanding_peak",
                            "limit_drop"},
                           "retained_shm_receive", topic, type, driver)) {
    return false;
  }
  reporter.delivered = slots[0];
  reporter.callback_entered = slots[1];
  reporter.callback_completed = slots[2];
  reporter.callback_threw_std = slots[3];
  reporter.callback_threw_nonstd = slots[4];
  reporter.released = slots[5];
  reporter.outstanding = slots[6];
  reporter.outstanding_peak = slots[7];
  reporter.limit_drop = slots[8];
  return true;
}

// ShmSubscriptionOptions::reporter_state of a retained subscription. The
// counters below the transaction are only touched under the subscription's
// mutex.
struct SubscriptionReporterState {
  ShmReceiveReporter reporter;
  ReporterBatchRegistration transaction;
  size_t reported_peak = 0;
};

// Hooks of ShmSubscription; `state` is its options' reporter_state.
inline void ReportReceive(void* state, ibmw_reporter_slot_v1* ShmReceiveReporter::*slot) noexcept {
  if (auto* s = static_cast<SubscriptionReporterState*>(state)) ReporterAdd(s->reporter.api, s->reporter.*slot);
}

// A lease was taken (`acquired`) or given back; `outstanding` is the count
// after the change. Called under the subscription's mutex.
inline void ReportLeaseLocked(void* state, bool acquired, size_t outstanding) noexcept {
  auto* s = static_cast<SubscriptionReporterState*>(state);
  if (!s) return;
  const auto& r = s->reporter;
  ReporterAdd(r.api, acquired ? r.delivered : r.released);
  r.api->add_signed(r.api->owner, r.outstanding, acquired ? 1 : -1);
  if (acquired && outstanding > s->reported_peak) {
    ReporterAdd(r.api, r.outstanding_peak, outstanding - s->reported_peak);
    s->reported_peak = outstanding;
  }
}

// ShmTransportDriver's view of the host, validated once at Initialize().
struct DriverReporterState {
  const ibmw_reporter_v1* api = nullptr;
  const ibmw_reporter_batch_v1* batch = nullptr;
  std::string driver;
};

// Counts one ctx-meta loan call: attempt now, success or fail (and the growth
// retries) when it ends. No-op for a publisher without a reporter.
class DirectLoanReporterOutcome {
 public:
  explicit DirectLoanReporterOutcome(const void* reporter) noexcept
      : reporter_(static_cast<const DirectLoanReporter*>(reporter)) {
    if (reporter_) ReporterAdd(reporter_->api, reporter_->slots.attempt);
  }
  ~DirectLoanReporterOutcome() {
    if (!reporter_) return;
    if (retries != 0) ReporterAdd(reporter_->api, reporter_->slots.retry, retries);
    if (!success_) ReporterAdd(reporter_->api, reporter_->slots.fail);
  }
  DirectLoanReporterOutcome(const DirectLoanReporterOutcome&) = delete;
  DirectLoanReporterOutcome& operator=(const DirectLoanReporterOutcome&) = delete;

  bool Reported() const noexcept { return reporter_ != nullptr; }
  void MarkSuccess() noexcept {
    if (reporter_) ReporterAdd(reporter_->api, reporter_->slots.success);
    success_ = true;
  }

  size_t retries = 0;

 private:
  const DirectLoanReporter* reporter_;
  bool success_ = false;
};

// End of a reported loan handle: `published`, or `released` (given back or
// refused by the library). Neither for a stale token.
inline void ReportDirectLoanReturn(const void* reporter, bool published, bool released) noexcept {
  const auto* r = static_cast<const DirectLoanReporter*>(reporter);
  if (!r) return;
  if (published) {
    ReporterAdd(r->api, r->slots.publish);
  } else if (released) {
    ReporterAdd(r->api, r->slots.release);
  }
}

}  // namespace ibmw::extensions::shm_extension

#endif
