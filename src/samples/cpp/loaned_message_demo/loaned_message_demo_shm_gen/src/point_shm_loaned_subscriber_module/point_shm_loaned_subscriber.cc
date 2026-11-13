// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "point_shm_loaned_subscriber_module/point_shm_loaned_subscriber.h"

// Include the transport-specific typesupport translation unit.
// This carries every dds/iceoryx-specific symbol referenced below
// (subscriber handle binding, attach callback, default OnMessage body).
#include "point_shm_loaned_subscriber_module/point_shm_loaned_subscriber_typesupport.inc.cc"

namespace ibmw::examples::loaned_message_demo_shm_gen {

bool PointShmLoanedSubscriber::Setup(ibmw::Runtime core) {
  core_ = core;

  // Plugin-injected: expands to GetSubscriber + OnMessage<T> /
  // LoanedSubscriber<T>::AttachBytes registration that forwards to
  // the user OnMessage virtual after incrementing received_.
  IBMW_TS_LOANED_SUB_ATTACH(core_);

  MW_INFO("PointShmLoanedSubscriber::Setup succeeded.");
  return true;
}

bool PointShmLoanedSubscriber::Run() {
  // Nothing to do — deliveries already flow through the callback
  // installed in Setup(). No driver pointer needed.
  MW_INFO("PointShmLoanedSubscriber::Run started.");
  return true;
}

void PointShmLoanedSubscriber::Stop() {
  MW_INFO("PointShmLoanedSubscriber::Stop succeeded (received={}, dropped={}).",
          received_.load(), dropped_.load());
}

// The default OnMessage implementation body is injected by the plugin
// via the transport-specific inc.cc included above (it carries the
// `on_message_body` user-code marker).

}  // namespace ibmw::examples::loaned_message_demo_shm_gen
