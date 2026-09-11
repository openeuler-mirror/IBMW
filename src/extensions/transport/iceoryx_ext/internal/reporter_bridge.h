#pragma once

#include <memory>
#include <string_view>
#include <utility>

namespace ibmw::runtime::core::transport {
struct TransportInfo;
}

namespace ibmw::extensions::iceoryx_extension {

namespace internal {
template <typename Prepare, typename Rollback>
bool PreparePublisherRegistration(bool created, Prepare&& prepare,
                                  Rollback&& rollback) noexcept {
  if (std::forward<Prepare>(prepare)()) return true;
  if (created) std::forward<Rollback>(rollback)();
  return false;
}
}  // namespace internal

class IceoryxTransportDriver;
class IoxPublisher;
class IoxSubscriptionState;
struct IoxSubscriberRegistrationOptions;

void* CreateDriverReporterState() noexcept;
void DestroyDriverReporterState(void*) noexcept;
bool DriverReporterActive(const void*) noexcept;
bool PreparePublisherReporter(
    void*, IoxPublisher*, const runtime::core::transport::TransportInfo&,
    bool created) noexcept;
bool PrepareSubscriptionReporter(
    void*, IoxSubscriberRegistrationOptions&, std::string_view,
    std::string_view) noexcept;
bool CommitSubscriptionReporter(IoxSubscriptionState*) noexcept;
void ResetSubscriptionReporter(IoxSubscriptionState*) noexcept;
void DestroySubscriptionReporter(IoxSubscriptionState*) noexcept;
void DestroyPublisherReporter(IoxPublisher*) noexcept;

}  // namespace ibmw::extensions::iceoryx_extension
