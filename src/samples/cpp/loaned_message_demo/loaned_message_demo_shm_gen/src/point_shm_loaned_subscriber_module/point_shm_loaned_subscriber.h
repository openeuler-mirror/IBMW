// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cstdint>

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "point_shm_loaned_subscriber_typesupport.inc.h"

namespace ibmw::examples::loaned_message_demo_shm_gen {

class PointShmLoanedSubscriber : public ibmw::Module {
 public:
  PointShmLoanedSubscriber() = default;
  ~PointShmLoanedSubscriber() override = default;

  // Plugin-injected: iceoryx expands to static_assert trivially-
  // copyable; dds expands to a no-op.
  IBMW_TS_LOANED_SUB_STATIC_ASSERT;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PointShmLoanedSubscriber"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 protected:
  // Plugin-injected: virtual user-callback declaration. Signatures
  // intentionally diverge — dds uses shared_ptr<const T>, iceoryx uses
  // (const T&, std::size_t size). This keeps byte-semantic equivalence
  // with the legacy generated code.
  IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL;

  auto GetLogger() { return core_.GetLogger(); }

 private:
  ibmw::Runtime core_;
  std::atomic<uint64_t> received_{0};
  std::atomic<uint64_t> dropped_{0};
};

}  // namespace ibmw::examples::loaned_message_demo_shm_gen
