// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R8 validation sample: v2 subscriber_for<T>() factory API only.
// Symmetric counterpart to direct_iceoryx_publisher_ok; see that sample's
// header comment for the full rationale (type reuse, R1/R2 contract,
// non-movable storage note).

#include <string_view>
#include <utility>

#include "cpp/core.h"
#include "cpp/transport/transport_factory.h"

#include "bench_plain_payloads.h"

namespace {
using R8ValidationMsg =
    ::ibmw::samples::transport_benchmark::BenchPlainPayload1KB;
}

class SubscriberOkModule {
 public:
  using v2_driver_slot_marker =
      decltype(ibmw::transport::subscriber_for<R8ValidationMsg>(
          std::declval<ibmw::Runtime&>(), std::string_view{}));

  void Setup(ibmw::Runtime& rt) {
    auto receiver =
        ibmw::transport::subscriber_for<R8ValidationMsg>(rt, "/r8_topic");
    (void)receiver;
  }
};

int main() { return 0; }
