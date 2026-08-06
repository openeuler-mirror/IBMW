// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <memory>

#include "engine/transport/transport_catalog.h"

namespace ibmw::runtime::core::transport {

// Frozen V1 optional interfaces. Capability pointers are borrowed and remain
// valid only for the lifetime of the TransportDriver registered with them.
class TransportShutdownCapabilityV1 {
 public:
  // Phase one of shutdown. Implementations must atomically stop accepting new
  // work, stop intake, and wait for callbacks and loans to quiesce. A timeout
  // must not destroy resources: the operation remains retryable, already
  // prepared implementations are idempotent, and existing loans must still be
  // publishable or releasable while preparation is pending.
  virtual bool TryPrepareShutdown(
      std::chrono::milliseconds timeout) noexcept = 0;

  // A prompt, side-effect-free veto for callback or otherwise unsafe threads.
  virtual bool CanShutdownFromCurrentThread() const noexcept = 0;

 protected:
  virtual ~TransportShutdownCapabilityV1() = default;
};

class LoanedPodRegistrationCapabilityV1 {
 public:
  virtual bool RegisterLoanedPodTopic(const TransportInfo& info,
                                      size_t initial_size_hint,
                                      size_t alignment) noexcept = 0;

 protected:
  virtual ~LoanedPodRegistrationCapabilityV1() = default;
};

class RetainedLoanCapabilityV1 {
 public:
  virtual std::unique_ptr<LoanedSubscriptionHandle> AttachLoanedRetained(
      const LoanedMessageHandlerWrapper& message_handler,
      RetainedLoanOptions options) noexcept = 0;

 protected:
  virtual ~RetainedLoanCapabilityV1() = default;
};

// Frozen V1 bundle layout. All pointers are borrowed from the registered
// TransportDriver; never delete an object through a capability pointer.
struct TransportDriverCapabilitiesV1 {
  TransportShutdownCapabilityV1* shutdown_v1 = nullptr;
  LoanedPodRegistrationCapabilityV1* loaned_pod_registration_v1 = nullptr;
  RetainedLoanCapabilityV1* retained_loan_v1 = nullptr;
};

}  // namespace ibmw::runtime::core::transport
