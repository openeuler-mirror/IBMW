// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

//
// Loaned Publisher (transport-agnostic). The generated TypeSupport fragment
// binds the stable SenderAccess<T> / LoanedSample<T> API to the message type;
// concrete transport selection remains in runtime configuration.
//
// The user-facing template remains transport-agnostic.

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>

#include "cpp/module_base.h"

// Message TypeSupport binding lives in the generated inc.h. It defines the
// seven IBMW_TS_LOANED_PUB_* macros referenced below using only public APIs.
#include "point_loaned_publisher_typesupport.inc.h"

namespace ibmw::examples::loaned_message_demo_gen {

class PointLoanedPublisher : public ibmw::Module {
 public:
  PointLoanedPublisher() = default;
  ~PointLoanedPublisher() override = default;

  // The public loaned API requires an in-place-safe message type.
  IBMW_TS_LOANED_PUB_STATIC_ASSERT;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "PointLoanedPublisher"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  void PrintStatistics();
  auto GetLogger() { return core_.GetLogger(); }

  ibmw::Runtime core_;

  // Plugin-injected stable publisher access for the selected message type.
  IBMW_TS_LOANED_PUB_MEMBER;

  std::thread publish_thread_;
  std::atomic<bool> running_{false};
  uint64_t publish_count_{0};
  uint64_t zero_copy_count_{0};
  uint64_t fallback_count_{0};
};

}  // namespace ibmw::examples::loaned_message_demo_gen
