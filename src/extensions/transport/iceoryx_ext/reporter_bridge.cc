#include "iceoryx_ext/internal/reporter_bridge.h"

#include "iceoryx_ext/iceoryx_bus.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "iceoryx_ext/internal/reporter_sidecar.h"

#include <cstddef>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace ibmw::extensions::iceoryx_extension {
namespace {
template <typename Api>
const Api* QueryHost(const char* symbol) noexcept {
#if defined(_WIN32)
  (void)symbol;
  return nullptr;
#else
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  if (!host) return nullptr;
  using Query = const Api* (*)(void);
  auto query = reinterpret_cast<Query>(dlsym(host, symbol));
  const Api* api = query ? query() : nullptr;
  dlclose(host);
  return api;
#endif
}
}  // namespace

void* CreateDriverReporterState() noexcept {
  const auto* api = QueryHost<ibmw_reporter_v1>("ibmw_reporter_query_v1");
  const auto* batch =
      QueryHost<ibmw_reporter_batch_v1>("ibmw_reporter_query_batch_v1");
  const bool valid_api =
      api && api->abi_version == IBMW_REPORTER_ABI_VERSION_V1 && api->owner &&
      api->struct_size >= offsetof(ibmw_reporter_v1, add) + sizeof(api->add) &&
      api->register_slot && api->add;
  const bool valid_batch =
      batch && batch->abi_version == IBMW_REPORTER_BATCH_ABI_VERSION_V1 &&
      batch->owner == (valid_api ? api->owner : nullptr) &&
      batch->struct_size >=
          offsetof(ibmw_reporter_batch_v1, abort) + sizeof(batch->abort) &&
      batch->prepare && batch->commit && batch->abort;
  if (!valid_api || !valid_batch) return nullptr;
  try {
    return new DriverReporterState{api, batch};
  } catch (...) {
    return nullptr;
  }
}

void DestroyDriverReporterState(void* state) noexcept {
  delete static_cast<DriverReporterState*>(state);
}
bool DriverReporterActive(const void* state) noexcept { return state != nullptr; }

bool PreparePublisherReporter(
    void* opaque, IoxPublisher* publisher,
    const runtime::core::transport::TransportInfo& info,
    bool created) noexcept {
  auto* driver = static_cast<DriverReporterState*>(opaque);
  if (!driver || !created) return true;
  try {
    ReporterBatchRegistration transaction;
    auto ops = std::make_unique<DirectPublisherOps>();
    ops->raw_loan = &DirectPublisherOps::RawLoanImpl<true>;
    ops->serialized_loan = &DirectPublisherOps::LoanImpl<true>;
    ops->publish = &DirectPublisherOps::CompleteImpl<true, true>;
    ops->release = &DirectPublisherOps::CompleteImpl<true, false>;
    ops->binding.api = driver->api;
    std::array<ibmw_reporter_slot_v1*, 6> staged{};
    if (!transaction.Prepare(
            driver->api, driver->batch, staged,
            std::array<std::string_view, 6>{
                "logical_attempt", "logical_success", "logical_fail",
                "internal_retry", "publish", "release"},
            "direct_iceoryx_loan", info.topic_name, info.msg_type)) return false;
    auto& slots = ops->binding.slots;
    slots.attempt = staged[0];
    slots.success = staged[1];
    slots.fail = staged[2];
    slots.retry = staged[3];
    slots.publish = staged[4];
    slots.release = staged[5];
    if (!ReporterSidecarAccess::PreparePublisherInstall(publisher) ||
        !transaction.Commit()) return false;
    ReporterSidecarAccess::InstallPublisher(publisher, std::move(ops));
    return true;
  } catch (...) {
    return false;
  }
}

bool PrepareSubscriptionReporter(
    void* opaque, IoxSubscriberRegistrationOptions& options,
    std::string_view topic, std::string_view type) noexcept {
  auto* driver = static_cast<DriverReporterState*>(opaque);
  if (!driver) return true;
  try {
    auto state = std::make_unique<SubscriptionReporterState>();
    auto reporter = std::make_unique<IoxReceiveReporter>();
    state->transaction = std::make_shared<ReporterBatchRegistration>();
    const bool has_add_signed =
        driver->api->struct_size >=
            offsetof(ibmw_reporter_v1, add_signed) +
                sizeof(driver->api->add_signed) &&
        driver->api->add_signed;
    if (!has_add_signed ||
        !PrepareIoxReceiveReporterSlots(
            driver->api, driver->batch, *state->transaction,
            *reporter, topic, type)) {
      return false;
    }
    state->reporter = std::move(reporter);
    options.reporter_state = std::shared_ptr<void>(
        state.release(), [](void* value) {
          delete static_cast<SubscriptionReporterState*>(value);
        });
    return true;
  } catch (...) {
    return false;
  }
}

PublisherReporterState* ReporterSidecarAccess::Publisher(
    IoxPublisher* publisher) noexcept {
  return static_cast<PublisherReporterState*>(publisher->reporter_state_);
}
const PublisherReporterState* ReporterSidecarAccess::Publisher(
    const IoxPublisher* publisher) noexcept {
  return static_cast<const PublisherReporterState*>(publisher->reporter_state_);
}
SubscriptionReporterState* ReporterSidecarAccess::Subscription(
    IoxSubscriptionState* subscription) noexcept {
  return static_cast<SubscriptionReporterState*>(
      subscription->reporter_state_.get());
}
const DirectPublisherOps* ReporterSidecarAccess::DirectOps(
    const IoxPublisher* publisher) noexcept {
  const auto* state = Publisher(publisher);
  return state ? state->active.load(std::memory_order_acquire) : nullptr;
}
bool ReporterSidecarAccess::PreparePublisherInstall(
    IoxPublisher* publisher) noexcept {
  try {
    auto* state = Publisher(publisher);
    if (!state) {
      publisher->reporter_state_ = new PublisherReporterState;
      state = Publisher(publisher);
    }
    state->storage.reserve(state->storage.size() + 1);
    return true;
  } catch (...) {
    return false;
  }
}
void ReporterSidecarAccess::InstallPublisher(
    IoxPublisher* publisher,
    std::unique_ptr<const DirectPublisherOps> direct_ops) noexcept {
  auto* state = Publisher(publisher);
  if (!state || !direct_ops) return;
  const auto* active = direct_ops.get();
  state->storage.push_back(std::move(direct_ops));
  state->active.store(active, std::memory_order_release);
}

void DestroyPublisherReporter(IoxPublisher* publisher) noexcept {
  delete ReporterSidecarAccess::Publisher(publisher);
}
void DestroySubscriptionReporter(IoxSubscriptionState* subscription) noexcept {
  (void)subscription;
}
void ResetSubscriptionReporter(IoxSubscriptionState* subscription) noexcept {
  if (auto* state = ReporterSidecarAccess::Subscription(subscription))
    state->transaction.reset();
}
bool CommitSubscriptionReporter(IoxSubscriptionState* subscription) noexcept {
  auto* state = ReporterSidecarAccess::Subscription(subscription);
  return !state || !state->transaction || !state->transaction->Active() ||
         state->transaction->Commit();
}

}  // namespace ibmw::extensions::iceoryx_extension
