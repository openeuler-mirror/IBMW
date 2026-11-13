// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_discovery_provider.h"
#include "zenoh_discovery_util.h"
#include "../key_expr.h"

#include <map>
#include <mutex>
#include <thread>

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
using zenoh_detail::Check;
using zenoh_detail::Owned;

class Provider final : public DiscoveryProvider {
 public:
  ~Provider() override { Stop(); }

  void Start(const DiscoveryOptions& requested) override {
    if (z_internal_check(session_.value)) throw std::logic_error("Zenoh discovery is already started.");
    if (requested.backend != DiscoveryBackend::kZenoh) throw std::invalid_argument("Invalid Zenoh discovery source.");
    // Same domain spelling as the drivers' limit_domain: surrounding slashes ignored (TRANS-047).
    auto options = requested;
    options.zenoh_domain = extensions::zenoh_extension::NormalizeLimitDomain(requested.zenoh_domain);
    source_ = DescribeSource(options);
    selector_ = zenoh_protocol::DomainPrefix(options.zenoh_domain) + "*/snapshot";
    zenoh_detail::OpenSession(session_.value, options.zenoh_config);
    worker_ = std::jthread([this](std::stop_token stop) { Collect(stop); });
  }

  DiscoverySnapshot Query(std::chrono::steady_clock::time_point deadline) override {
    if (!worker_.joinable()) throw std::logic_error("Zenoh discovery is not started.");
    std::this_thread::sleep_until(deadline);
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(snapshots_, [&](const auto& item) { return now - item.second.last_seen > 2s; });
    DiscoverySnapshot result;
    for (const auto& [instance, snapshot] : snapshots_) {
      result.endpoints.insert(result.endpoints.end(), snapshot.snapshot.endpoints.begin(), snapshot.snapshot.endpoints.end());
    }
    result.sources.push_back({.source = source_,
        .status = error_.empty() ? DiscoveryStatus::kReady : DiscoveryStatus::kError,
        .message = error_, .observed_at = std::chrono::system_clock::now()});
    return result;
  }

  void Stop() noexcept override {
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
    if (z_internal_check(session_.value)) {
      z_close(z_loan_mut(session_.value), nullptr);
      z_drop(z_move(session_.value));
    }
    std::lock_guard lock(mutex_);
    snapshots_.clear();
    error_.clear();
  }

 private:
  void SetError(const std::string& message) {
    std::lock_guard lock(mutex_);
    if (error_.empty()) error_ = message;
  }

  void Accept(const z_loaned_reply_t* reply) {
    if (!z_reply_is_ok(reply)) throw std::runtime_error("Zenoh discovery responder returned an error.");
    const auto* sample = z_reply_ok(reply);
    z_view_string_t key;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
    auto snapshot = zenoh_protocol::Decode(zenoh_detail::ReadBytes(z_sample_payload(sample)),
        {z_string_data(z_loan(key)), z_string_len(z_loan(key))}, source_);
    std::lock_guard lock(mutex_);
    const auto previous = snapshots_.find(snapshot.instance);
    if (previous == snapshots_.end() || previous->second.snapshot.revision <= snapshot.revision) {
      // Replace whole instance snapshots so removed entries cannot accumulate.
      const auto instance = snapshot.instance;
      snapshots_.insert_or_assign(instance, Observed{std::move(snapshot), std::chrono::steady_clock::now()});
    }
  }

  void Collect(std::stop_token stop) noexcept {
    try {
      z_view_keyexpr_t key;
      Check(z_view_keyexpr_from_str(&key, selector_.c_str()), "Create Zenoh discovery selector");
      while (!stop.stop_requested()) {
        const auto next = std::chrono::steady_clock::now() + 200ms;
        Owned<z_owned_closure_reply_t> callback;
        Owned<z_owned_fifo_handler_reply_t> replies;
        z_fifo_channel_reply_new(&callback.value, &replies.value, 64);
        z_get_options_t options;
        z_get_options_default(&options);
        options.target = Z_QUERY_TARGET_ALL;
        options.consolidation = z_query_consolidation_none();
        options.timeout_ms = 200;
        Check(z_get(z_loan(session_.value), z_loan(key), "", z_move(callback.value), &options), "Query Zenoh discovery metadata");
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < next) {
          Owned<z_owned_reply_t> reply;
          const auto code = z_fifo_handler_reply_try_recv(z_loan(replies.value), &reply.value);
          if (code == Z_OK) {
            try { Accept(z_loan(reply.value)); }
            catch (const std::exception& error) { SetError(error.what()); }
          } else if (code == Z_CHANNEL_NODATA || code == Z_CHANNEL_DISCONNECTED) {
            std::this_thread::sleep_for(10ms);
          } else {
            Check(code, "Receive Zenoh discovery reply");
          }
        }
        // Repeat to allow a newly opened session's routes to converge. FIFO
        // closures run inside zenoh-c; none retain a pointer to the plugin.
      }
    } catch (const std::exception& error) {
      SetError(error.what());
    } catch (...) {
      SetError("Unknown Zenoh discovery failure.");
    }
  }

  DiscoverySource source_;
  std::string selector_;
  Owned<z_owned_session_t> session_;
  std::jthread worker_;
  std::mutex mutex_;
  struct Observed {
    zenoh_protocol::Snapshot snapshot;
    std::chrono::steady_clock::time_point last_seen;
  };
  std::map<std::string, Observed> snapshots_;
  std::string error_;
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::DiscoveryPluginV1 IbmwDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = DiscoveryBackend::kZenoh,
          .create = [] { return static_cast<DiscoveryProvider*>(new Provider); },
          .destroy = [](DiscoveryProvider* provider) noexcept { delete provider; }};
}
