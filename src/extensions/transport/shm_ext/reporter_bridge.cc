// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "shm_ext/internal/reporter_bridge.h"

#include <cstddef>
#include <dlfcn.h>

#include "engine/transport/message_frame.h"
#include "shm_ext/internal/reporter_sidecar.h"
#include "shm_ext/shm_bus.h"

namespace ibmw::extensions::shm_extension {
namespace {

// The host exports the query functions (ENABLE_EXPORTS); a plugin loaded with
// RTLD_DEEPBIND still reaches the host's owner through the global scope.
template <typename Api>
const Api* QueryHost(const char* symbol) noexcept {
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  if (!host) return nullptr;
  using Query = const Api* (*)(void);
  auto query = reinterpret_cast<Query>(dlsym(host, symbol));
  const Api* api = query ? query() : nullptr;
  dlclose(host);
  return api;
}

bool ValidReporter(const ibmw_reporter_v1* api) noexcept {
  return api && api->abi_version == IBMW_REPORTER_ABI_VERSION_V1 && api->owner &&
         api->struct_size >= offsetof(ibmw_reporter_v1, add) + sizeof(api->add) && api->register_slot && api->add;
}

bool HasAddSigned(const ibmw_reporter_v1* api) noexcept {
  return api->struct_size >= offsetof(ibmw_reporter_v1, add_signed) + sizeof(api->add_signed) && api->add_signed;
}

}  // namespace

std::shared_ptr<void> CreateDriverReporterState(std::string_view driver_name) noexcept {
  const auto* api = QueryHost<ibmw_reporter_v1>("ibmw_reporter_query_v1");
  const auto* batch = QueryHost<ibmw_reporter_batch_v1>("ibmw_reporter_query_batch_v1");
  if (!ValidReporter(api) || !ValidReporterBatch(batch) || batch->owner != api->owner) return nullptr;
  try {
    return std::make_shared<DriverReporterState>(DriverReporterState{api, batch, std::string(driver_name)});
  } catch (...) {
    return nullptr;
  }
}

bool PreparePublisherReporter(void* driver_state, ShmPublisher& publisher,
                              const runtime::core::transport::TransportInfo& info) noexcept {
  const auto* driver = static_cast<const DriverReporterState*>(driver_state);
  if (!driver) return true;
  try {
    ReporterBatchRegistration transaction;
    auto reporter = std::make_shared<DirectLoanReporter>();
    if (!PrepareDirectLoanReporterSlots(driver->api, driver->batch, transaction, *reporter, info.topic_name,
                                        info.msg_type, driver->driver) ||
        !transaction.Commit()) {
      return false;
    }
    publisher.SetReporter(std::move(reporter));
    return true;
  } catch (...) {
    return false;
  }
}

bool PrepareSubscriptionReporter(void* driver_state, ShmSubscriptionOptions& options, std::string_view topic,
                                 std::string_view type) noexcept {
  const auto* driver = static_cast<const DriverReporterState*>(driver_state);
  if (!driver) return true;
  // outstanding_current is signed: without add_signed the family is incomplete.
  if (!HasAddSigned(driver->api)) return false;
  try {
    auto state = std::make_shared<SubscriptionReporterState>();
    if (!PrepareShmReceiveReporterSlots(driver->api, driver->batch, state->transaction, state->reporter, topic,
                                        type, driver->driver)) {
      return false;
    }
    options.reporter_state = std::move(state);
    return true;
  } catch (...) {
    return false;
  }
}

bool CommitSubscriptionReporter(const ShmSubscriptionOptions& options) noexcept {
  auto* state = static_cast<SubscriptionReporterState*>(options.reporter_state.get());
  if (!state || !state->transaction.Active() || state->transaction.Commit()) return true;
  state->transaction.Abort();
  return false;
}

}  // namespace ibmw::extensions::shm_extension

// Host reporter as this plugin sees it (nullptr when off or invalid); lets a
// test check that a plugin loaded with RTLD_DEEPBIND reaches the host owner.
extern "C" IBMW_SHM_EXPORT const ibmw_reporter_v1* ibmw_shm_query_host_reporter_v1(void) noexcept {
  const auto* api = ibmw::extensions::shm_extension::QueryHost<ibmw_reporter_v1>("ibmw_reporter_query_v1");
  return ibmw::extensions::shm_extension::ValidReporter(api) ? api : nullptr;
}
