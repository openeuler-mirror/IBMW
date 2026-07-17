// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "point_loaned_publisher_module/point_loaned_publisher.h"

// Include the transport-specific typesupport translation unit. It
// carries every dds/iceoryx-specific symbol referenced by the macro
// expansions below (publisher handle, driver-aware create call,
// LoanedSample borrow/publish, driver stats). The user-code marker
// `populate_loaned_message` lives inside the inc.h hot-loop macro
// definition so users edit one location regardless of transport.
#include "point_loaned_publisher_module/point_loaned_publisher_typesupport.inc.cc"

namespace ibmw::examples::loaned_message_demo_gen {

bool PointLoanedPublisher::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: GetPublisher + RegisterSendType<T> (+ compile-time
  // plain-type log on dds, sizeof log on iceoryx). Returns false on
  // registration failure.
  IBMW_TS_LOANED_PUB_SETUP(core_);

  MW_INFO("PointLoanedPublisher::Setup succeeded.");
  return true;
}

bool PointLoanedPublisher::Run() {
  // Plugin-injected: validates the cached driver pointer was injected
  // (SetDriverSlot / SetXxxDriver must have been called between
  // Initialize() and Start()) and performs driver-aware publisher
  // construction. Returns false on missing driver / construction
  // failure.
  IBMW_TS_LOANED_PUB_RUN_INIT(core_);

  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("PointLoanedPublisher::Run succeeded.");
  return true;
}

void PointLoanedPublisher::Stop() {
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  PrintStatistics();
  MW_INFO("PointLoanedPublisher::Stop succeeded (total published: {}).", publish_count_);
}

void PointLoanedPublisher::PublishLoop() {
  // Plugin-injected: WHOLE while-body. dds expands to borrow +
  // is_zero_copy three-counter + populate marker + publish + sleep;
  // iceoryx expands to LoanedSample::Borrow + strict mode +
  // populate marker + Publish + sleep. See plan §3.a decision —
  // single-macro hot loop is the only way to keep this user-facing
  // .cc transport-agnostic without forcing divergent control flow.
  while (running_) {
    IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_iter);
  }
}

void PointLoanedPublisher::PrintStatistics() {
  // Plugin-injected: dds emits 4 stat lines + 4 driver
  // GetZeroCopyStats lines + separator; iceoryx emits 4 stat lines
  // + strict-mode comment + separator.
  IBMW_TS_LOANED_PUB_PRINT_STATS;
}

}  // namespace ibmw::examples::loaned_message_demo_gen
