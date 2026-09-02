#include "iceoryx_ext/internal/reporter_bridge.h"

namespace ibmw::extensions::iceoryx_extension {
void* CreateDriverReporterState() noexcept { return nullptr; }
void DestroyDriverReporterState(void*) noexcept {}
bool DriverReporterActive(const void*) noexcept { return false; }
bool PreparePublisherReporter(
    void*, IoxPublisher*,
    const runtime::core::transport::TransportInfo&, bool) noexcept {
  return true;
}
bool PrepareSubscriptionReporter(void*, IoxSubscriberRegistrationOptions&,
                                 std::string_view, std::string_view) noexcept {
  return true;
}
bool CommitSubscriptionReporter(IoxSubscriptionState*) noexcept { return true; }
void ResetSubscriptionReporter(IoxSubscriptionState*) noexcept {}
void DestroySubscriptionReporter(IoxSubscriptionState*) noexcept {}
void DestroyPublisherReporter(IoxPublisher*) noexcept {}
}  // namespace ibmw::extensions::iceoryx_extension
