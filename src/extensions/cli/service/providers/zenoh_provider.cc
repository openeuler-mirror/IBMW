// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include <algorithm>
#include <map>
#include <thread>
#include "engine/discovery/service_discovery_protocol.h"
#include "zenoh_ext/discovery/zenoh_discovery_util.h"

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
using zenoh_detail::Check;
using zenoh_detail::Owned;
class Provider final : public ServiceDiscoveryProvider {
 public:
  ~Provider() override {
    if (z_internal_check(session_.value)) z_close(z_loan_mut(session_.value), nullptr);
  }
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    const auto source = DescribeSource(options);
    Owned<z_owned_config_t> config;
    if (options.zenoh_config.empty()) z_config_default(&config.value);
    else Check(zc_config_from_file(&config.value, options.zenoh_config.c_str()), "Load Zenoh configuration");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) throw std::runtime_error("Zenoh service discovery timed out.");
    const auto timeout = std::to_string(remaining);
    // Native profiles can request infinite connection retries. Bound this
    // observer's session setup by the CLI deadline, without changing the file.
    for (const auto* path : {"connect/timeout_ms", "listen/timeout_ms", "scouting/timeout"})
      Check(zc_config_insert_json5(z_loan_mut(config.value), path, timeout.c_str()), "Set Zenoh discovery timeout");
    const auto delay = std::to_string(std::min<int64_t>(500, remaining / 2));
    Check(zc_config_insert_json5(z_loan_mut(config.value), "scouting/delay", delay.c_str()), "Set Zenoh scouting delay");
    Check(z_open(&session_.value, z_move(config.value), nullptr), "Open Zenoh service discovery session");
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Zenoh service discovery timed out during session setup.");
    const auto selector = service_protocol::ZenohPrefix(options.zenoh_domain) + "*";
    z_view_keyexpr_t key;
    Check(z_view_keyexpr_from_str(&key, selector.c_str()), "Create service discovery selector");
    std::map<std::string, ServiceDiscoverySnapshot> snapshots;
    std::string failure;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto next = std::min(deadline, std::chrono::steady_clock::now() + 200ms);
      Owned<z_owned_closure_reply_t> callback;
      Owned<z_owned_fifo_handler_reply_t> replies;
      z_fifo_channel_reply_new(&callback.value, &replies.value, 64);
      z_get_options_t query_options;
      z_get_options_default(&query_options);
      query_options.target = Z_QUERY_TARGET_ALL;
      query_options.consolidation = z_query_consolidation_none();
      query_options.timeout_ms = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count());
      Check(z_get(z_loan(session_.value), z_loan(key), "", z_move(callback.value), &query_options), "Query Zenoh service directory");
      while (std::chrono::steady_clock::now() < next) {
        Owned<z_owned_reply_t> reply;
        const auto code = z_fifo_handler_reply_try_recv(z_loan(replies.value), &reply.value);
        if (code == Z_OK) {
          try {
            if (!z_reply_is_ok(z_loan(reply.value))) throw std::runtime_error("Zenoh service directory returned an error.");
            const auto* sample = z_reply_ok(z_loan(reply.value));
            auto snapshot = service_protocol::Decode(zenoh_detail::ReadBytes(z_sample_payload(sample)), source);
            if (!snapshot.services.empty()) {
              const auto instance = snapshot.services.front().runtime_id;
              snapshots.insert_or_assign(instance, std::move(snapshot));
            }
          } catch (const std::exception& error) { failure = error.what(); }
        } else if (code == Z_CHANNEL_NODATA || code == Z_CHANNEL_DISCONNECTED) {
          std::this_thread::sleep_for(5ms);
        } else { Check(code, "Receive Zenoh service directory"); }
      }
    }
    ServiceDiscoverySnapshot result;
    for (const auto& [instance, snapshot] : snapshots)
      result.services.insert(result.services.end(), snapshot.services.begin(), snapshot.services.end());
    result.sources.push_back({.source = source, .status = failure.empty() ? DiscoveryStatus::kReady : DiscoveryStatus::kError, .message = failure});
    return result;
  }
 private:
  Owned<z_owned_session_t> session_;
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kZenoh,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
