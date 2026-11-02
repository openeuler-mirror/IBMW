// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "observability/core_metrics.h"

namespace ibmw::runtime::core::observability {

void Registry::ResetReporterLocked() { reporter_state_ = nullptr; }
void Registry::DestroyReporter() noexcept { reporter_state_ = nullptr; }
void Registry::AppendReporterJsonlLocked(std::ostringstream&, uint64_t&) const {}
size_t Registry::ReporterStorageSizeLocked() const noexcept { return 0; }
Registry::ReporterSlot* Registry::RegisterReporterSlot(
    const ibmw_reporter_slot_descriptor_v1*) { return nullptr; }
bool Registry::PrepareReporterBatch(const ibmw_reporter_slot_descriptor_v1*,
                                    size_t, ibmw_reporter_slot_v1**,
                                    ibmw_reporter_batch_transaction_v1**) {
  return false;
}
bool Registry::CommitReporterBatch(ibmw_reporter_batch_transaction_v1*) {
  return false;
}
void Registry::AbortReporterBatch(ibmw_reporter_batch_transaction_v1*) noexcept {}
bool Registry::HasPendingReporterBatches() const noexcept { return false; }
uint64_t Registry::ReporterValueForTesting(const ReporterSlot*) const noexcept {
  return 0;
}

Registry::ReporterSlot* Controller::RegisterReporterSlot(
    const ibmw_reporter_slot_descriptor_v1*) noexcept { return nullptr; }
bool Controller::PrepareReporterBatch(
    const ibmw_reporter_slot_descriptor_v1*, size_t, ibmw_reporter_slot_v1**,
    ibmw_reporter_batch_transaction_v1**) noexcept { return false; }
bool Controller::CommitReporterBatch(
    ibmw_reporter_batch_transaction_v1*) noexcept { return false; }
void Controller::AbortReporterBatch(
    ibmw_reporter_batch_transaction_v1*) noexcept {}

}  // namespace ibmw::runtime::core::observability
