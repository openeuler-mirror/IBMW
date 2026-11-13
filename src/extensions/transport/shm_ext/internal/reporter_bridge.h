// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string_view>

// Reporter V1 bridge of the shm transport (see internal/reporter_sidecar.h for
// the slots). reporter_bridge.cc with IBMW_ENABLE_REPORTER_V1, else the no-op
// reporter_bridge_stub.cc. All state is opaque here so the public headers do
// not depend on the build switch.
namespace ibmw::runtime::core::transport {
struct TransportInfo;
}

namespace ibmw::extensions::shm_extension {

class ShmPublisher;
struct ShmSubscriptionOptions;

// Host reporter and batch API, validated; nullptr when the host has none
// (switched off at build or run time), which turns reporting off.
std::shared_ptr<void> CreateDriverReporterState(std::string_view driver_name) noexcept;
// Installs the direct_shm_loan slots on a publisher this registration creates,
// before it is published. True when there is nothing to do; false when
// reporting is on and the slots cannot be registered (the caller fails the
// registration).
bool PreparePublisherReporter(void* driver_state, ShmPublisher& publisher,
                              const runtime::core::transport::TransportInfo& info) noexcept;
// Stages the retained_shm_receive slots in `options.reporter_state`; false
// when reporting is on and they cannot be staged.
bool PrepareSubscriptionReporter(void* driver_state, ShmSubscriptionOptions& options, std::string_view topic,
                                 std::string_view type) noexcept;
// Commits what PrepareSubscriptionReporter staged; on failure the slots are
// aborted and false is returned.
bool CommitSubscriptionReporter(const ShmSubscriptionOptions& options) noexcept;

}  // namespace ibmw::extensions::shm_extension
