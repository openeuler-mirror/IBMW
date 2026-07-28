// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher.h"

// Include the transport-specific typesupport translation unit. It
// carries every dds/iceoryx-specific symbol referenced by the macro
// expansions below (publisher handle, driver-aware create call,
// LoanedSample borrow/publish, driver stats). The user-code marker
// `populate_loaned_message` lives inside the inc.h hot-loop macro
// definition so users edit one location regardless of transport.
#include "point_iceoryx_loaned_publisher_module/point_iceoryx_loaned_publisher_typesupport.inc.cc"

namespace ibmw::examples::loaned_message_demo_iceoryx_gen {

bool PointIceoryxLoanedPublisher::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: GetPublisher + RegisterSendType<T> (+ compile-time
  // plain-type log on dds, sizeof log on iceoryx). Returns false on
  // registration failure.
  IBMW_TS_LOANED_PUB_SETUP(core_);

  MW_INFO("PointIceoryxLoanedPublisher::Setup succeeded.");
  return true;
}

bool PointIceoryxLoanedPublisher::Run() {
  // Plugin-injected validation of the stable publisher access.
  IBMW_TS_LOANED_PUB_RUN_INIT(core_);

  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("PointIceoryxLoanedPublisher::Run succeeded.");
  return true;
}

void PointIceoryxLoanedPublisher::Stop() {
  running_ = false;
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
  PrintStatistics();
  MW_INFO("PointIceoryxLoanedPublisher::Stop succeeded (total published: {}).", publish_count_);
}

void PointIceoryxLoanedPublisher::PublishLoop() {
  // Plugin-injected strict loan / populate / publish loop.
  while (running_) {
    IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_iter);
  }
}

void PointIceoryxLoanedPublisher::PrintStatistics() {
  // Plugin-injected transport-neutral counters.
  IBMW_TS_LOANED_PUB_PRINT_STATS;
}

}  // namespace ibmw::examples::loaned_message_demo_iceoryx_gen
