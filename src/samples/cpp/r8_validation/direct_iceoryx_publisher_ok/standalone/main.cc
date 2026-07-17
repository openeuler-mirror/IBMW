// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R8 validation sample: v2 publisher_for<T>() factory API only.
//
// This sample asserts at compile time that a user can stand up a publisher
// purely through the public cpp/transport/transport_factory.h surface,
// without including any transport-private header (R1) and without naming
// any extension-private namespace symbol (R2). It also exercises the
// IBMW_USE_DRIVER seam by exposing v2_driver_slot_marker so static
// assertions in the v2 wiring path resolve to a complete type.
//
// Type system: we reuse the bench_payload BenchPlainPayload1KB type which
// is already DDS-registered via the public IBMW_DDS_REGISTER_CONTEXT_TYPE
// umbrella (see bench_payload/bench_plain_payloads.h). That registration
// supplies the SenderAccess<T> specialization that publisher_for<T>()
// returns, per the transport_factory.h consumer-includes-typesupport
// public API invariant. Inventing a new
// local POD here would force this TU to either (a) include a transport-
// private typesupport header (R1 violation) or (b) hand-write the full
// IBMW_DDS_TYPE_NAME / FastCDR / IBMW_DDS_REGISTER_CONTEXT_TYPE /
// IBMW_DDS_PLAIN_TYPE boilerplate. Reusing an already-registered POD
// keeps the sample focused on what it asserts: that the v2 facade API
// itself is reachable from a user TU without any extension-private
// include or symbol name.
//
// Storage note: SenderAccess<T> is intentionally non-copyable and
// non-movable (the base deletes copy ops and the std::string member
// suppresses the implicit moves). Real modules keep a SenderAccess
// alive by emplacing it in transport-owned storage during the
// transport subsystem's bind step; that path is exercised by the
// transport_benchmark samples. For this compile-only assertion sample
// it is sufficient to instantiate the factory call as a local rvalue
// in Setup() -- the goal is to prove the symbol resolves and the
// returned type is complete on the user side.

#include <string_view>
#include <utility>

#include "cpp/core.h"
#include "cpp/transport/transport_factory.h"

#include "bench_plain_payloads.h"

namespace {
using R8ValidationMsg =
    ::ibmw::samples::transport_benchmark::BenchPlainPayload1KB;
}

class PublisherOkModule {
 public:
  using v2_driver_slot_marker =
      decltype(ibmw::transport::publisher_for<R8ValidationMsg>(
          std::declval<ibmw::Runtime&>(), std::string_view{}));

  void Setup(ibmw::Runtime& rt) {
    auto sender =
        ibmw::transport::publisher_for<R8ValidationMsg>(rt, "/r8_topic");
    (void)sender;
  }
};

int main() { return 0; }
