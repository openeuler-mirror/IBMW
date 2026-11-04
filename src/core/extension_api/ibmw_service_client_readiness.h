// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
#include <string_view>
namespace ibmw::extension {
// Optional capability, separate from the existing ServiceDriver vtable.
// Readiness is connection state, not an exactly-once delivery guarantee.
class ServiceClientReadinessV1 {
 public:
  virtual ~ServiceClientReadinessV1() = default;
  virtual bool IsClientReady(std::string_view module, std::string_view function) const noexcept = 0;
};
}
