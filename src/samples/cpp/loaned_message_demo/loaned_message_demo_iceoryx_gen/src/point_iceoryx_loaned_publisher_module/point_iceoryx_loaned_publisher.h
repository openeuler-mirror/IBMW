// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// Loaned Publisher (transport-agnostic) — driver-aware zero-copy
// publish. The actual transport mechanics (Ros2StylePublisher /
// LoanedSample / driver pointer / hot-loop borrow+publish) live in
// the generated <name>_typesupport.inc.{h,cc} alongside; user-facing
// .h/.cc only references those via the IBMW_TS_LOANED_PUB_* macros.
//
// The user-facing template remains transport-agnostic.

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h. It
// MUST #define all 8 IBMW_TS_LOANED_PUB_* macros referenced below.
// See <name>_typesupport.inc.h for the full hot-loop body and
// driver-injection details (the macros expand to multi-line code on
// purpose; reading the inc.h is the canonical way to understand the
// publish path).
#include "point_iceoryx_loaned_publisher_typesupport.inc.h"

namespace ibmw::examples::loaned_message_demo_iceoryx_gen {

class PointIceoryxLoanedPublisher : public ibmw::Module {
 public:
  PointIceoryxLoanedPublisher() = default;
  ~PointIceoryxLoanedPublisher() override = default;

  // Plugin-injected: iceoryx asserts trivially-copyable; dds expands
  // to a no-op (compile-time plain-type detection happens in Setup).
  IBMW_TS_LOANED_PUB_STATIC_ASSERT;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PointIceoryxLoanedPublisher"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

  // Plugin-injected: provides BOTH the named setter (preserves existing
  // app `main.cc` call sites — SetDdsDriver / SetIceoryxDriver) and the
  // engine-contract typed-slot override `SetDriverSlot(slot_id, void*)`.
  // The override matches the driver's stable slot id at compile time
  // and forwards via static_cast.
  IBMW_TS_LOANED_PUB_DRIVER_SETTER_DECL;

 private:
  void PublishLoop();
  void PrintStatistics();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;

  // Plugin-injected: declares the publisher handle (Ros2StylePublisher
  // shared_ptr / SenderRef) plus the cached driver pointer.
  IBMW_TS_LOANED_PUB_MEMBER;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint64_t publish_count_{0};
  uint64_t zero_copy_count_{0};
  uint64_t fallback_count_{0};
};

}  // namespace ibmw::examples::loaned_message_demo_iceoryx_gen
