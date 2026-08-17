#pragma once

#ifdef IBMW_ENABLE_REPORTER_V1

#include "extension_api/ibmw_reporter_v1.h"

namespace ibmw::extensions::iceoryx_extension {

struct IoxReceiveReporter {
  const ibmw_reporter_v1* api = nullptr;
  ibmw_reporter_slot_v1* delivered = nullptr;
  ibmw_reporter_slot_v1* callback_entered = nullptr;
  ibmw_reporter_slot_v1* callback_completed = nullptr;
  ibmw_reporter_slot_v1* callback_threw_std = nullptr;
  ibmw_reporter_slot_v1* callback_threw_nonstd = nullptr;
  ibmw_reporter_slot_v1* released = nullptr;
  ibmw_reporter_slot_v1* outstanding = nullptr;
  ibmw_reporter_slot_v1* outstanding_peak = nullptr;
  ibmw_reporter_slot_v1* limit_drop = nullptr;
};

class ScopedPendingIoxReceiveReporter {
 public:
  explicit ScopedPendingIoxReceiveReporter(IoxReceiveReporter* reporter) noexcept;
  ~ScopedPendingIoxReceiveReporter();

  ScopedPendingIoxReceiveReporter(const ScopedPendingIoxReceiveReporter&) = delete;
  ScopedPendingIoxReceiveReporter& operator=(const ScopedPendingIoxReceiveReporter&) = delete;

 private:
  IoxReceiveReporter* previous_;
};

}  // namespace ibmw::extensions::iceoryx_extension

#endif
