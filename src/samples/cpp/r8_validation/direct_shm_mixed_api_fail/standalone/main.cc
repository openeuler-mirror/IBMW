// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R8.2 validation sample: DELIBERATE BUILD FAILURE (shm_ext).
//
// shm_ext counterpart of direct_iceoryx_mixed_api_fail. This sample mixes
// the v2 publisher_for<T>() factory API with the source-compatible legacy
// shm_ext IoxStylePublisher<T> direct API in the same module.
// Instantiating IbmwR82SlotMixCheck<MixedApiModule> at namespace scope
// triggers the static_assert defined in shm_transport_driver.h.
//
// Because the demonstration requires naming legacy shm_ext symbols, every
// R1/R2/R3 violation site is wrapped in IBMW_R<N>_EXEMPT_BEGIN/END anchors
// so check_red_lines.sh ignores them. The companion sample
// direct_shm_no_anchor_fail intentionally OMITS those anchors.
//
// Gated by IBMW_BUILD_R8_FAIL_SAMPLES; default OFF in CI to keep the
// main build green. Exercised by the dual-build matrix entry
// r8_shm_mixed_api_fail.

#include <cstdint>
#include <utility>

#include "cpp/core.h"
#include "cpp/transport/transport_factory.h"

// IBMW_R1_EXEMPT_BEGIN
#include "shm_ext/iox_style_publisher.h"
#include "shm_ext/shm_transport_driver.h"
// IBMW_R1_EXEMPT_END

struct R8ValidationMsg {
  int32_t a;
  int32_t b;
};

// IBMW_R2_EXEMPT_BEGIN
namespace shm_ext = ibmw::extensions::shm_extension;
// IBMW_R2_EXEMPT_END

class MixedApiModule {
 public:
  // IBMW_R2_EXEMPT_BEGIN
  using legacy_iox_marker = shm_ext::IoxStylePublisher<R8ValidationMsg>;
  // IBMW_R2_EXEMPT_END
  using v2_driver_slot_marker =
      decltype(ibmw::transport::publisher_for<R8ValidationMsg>(
          std::declval<ibmw::Runtime&>(), std::string_view{}));

 private:
  // IBMW_R3_EXEMPT_BEGIN
  shm_ext::ShmTransportDriver* driver_{nullptr};
  // IBMW_R3_EXEMPT_END
};

// IBMW_R2_EXEMPT_BEGIN
static_assert(sizeof(shm_ext::IbmwR82SlotMixCheck<MixedApiModule>) >= 0,
              "instantiate trip wire");
// IBMW_R2_EXEMPT_END

int main() {
  return 0;
}
