// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Built without IBMW_ENABLE_REPORTER_V1: no host lookup, nothing reported.

#include "extension_api/ibmw_reporter_v1.h"
#include "shm_ext/internal/reporter_bridge.h"
#include "shm_ext/shm_bus.h"

namespace ibmw::extensions::shm_extension {
std::shared_ptr<void> CreateDriverReporterState(std::string_view) noexcept { return nullptr; }
bool PreparePublisherReporter(void*, ShmPublisher&, const runtime::core::transport::TransportInfo&) noexcept {
  return true;
}
bool PrepareSubscriptionReporter(void*, ShmSubscriptionOptions&, std::string_view, std::string_view) noexcept {
  return true;
}
bool CommitSubscriptionReporter(const ShmSubscriptionOptions&) noexcept { return true; }
}  // namespace ibmw::extensions::shm_extension

extern "C" IBMW_SHM_EXPORT const ibmw_reporter_v1* ibmw_shm_query_host_reporter_v1(void) noexcept { return nullptr; }
