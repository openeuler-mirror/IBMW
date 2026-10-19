// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_discovery_responder.h"

namespace ibmw::discovery {
using namespace std::chrono_literals;
using zenoh_detail::Check;
using zenoh_detail::Owned;

void ZenohDiscoveryResponder::Record(EndpointRole role, std::string topic, std::string type, std::string key) {
  std::lock_guard lock(mutex_);
  const auto identity = std::make_pair(role, key);
  if (endpoints_.contains(identity)) return;
  if (endpoints_.size() >= zenoh_protocol::kMaxEndpoints) throw std::runtime_error("Too many Zenoh discovery endpoints.");
  endpoints_.emplace(identity, DiscoveredEndpoint{
      .endpoint_id = std::to_string(++revision_), .role = role,
      .topic_name = std::move(topic), .type_name = std::move(type), .wire_topic_name = std::move(key)});
}

zenoh_protocol::Snapshot ZenohDiscoveryResponder::Snapshot() const {
  std::lock_guard lock(mutex_);
  zenoh_protocol::Snapshot result{.instance = instance_, .domain = domain_, .revision = revision_};
  for (const auto& [key, endpoint] : endpoints_) result.endpoints.push_back(endpoint);
  return result;
}

void ZenohDiscoveryResponder::Start(const z_loaned_session_t* session, std::string domain) {
  if (worker_.joinable()) throw std::logic_error("Zenoh discovery responder is already started.");
  instance_ = zenoh_detail::SessionId(session);
  domain_ = std::move(domain);
  key_ = zenoh_protocol::SnapshotKey(domain_, instance_);
  // Fail initialization for an unrepresentable catalog instead of silently
  // advertising an empty or truncated directory.
  static_cast<void>(zenoh_protocol::Encode(Snapshot()));
  z_view_keyexpr_t key;
  Check(z_view_keyexpr_from_str(&key, key_.c_str()), "Create Zenoh metadata key");
  Owned<z_owned_closure_query_t> callback;
  z_fifo_channel_query_new(&callback.value, &queries_.value, 16);
  Check(z_declare_queryable(session, &queryable_.value, z_loan(key), z_move(callback.value), nullptr),
        "Declare Zenoh discovery queryable");
  worker_ = std::jthread([this](std::stop_token stop) { Serve(stop); });
}

void ZenohDiscoveryResponder::Serve(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Owned<z_owned_query_t> query;
    const auto code = z_fifo_handler_query_try_recv(z_loan(queries_.value), &query.value);
    if (code == Z_CHANNEL_DISCONNECTED) return;
    if (code != Z_OK) {
      std::this_thread::sleep_for(10ms);
      continue;
    }
    try {
      const auto payload = zenoh_protocol::Encode(Snapshot());
      z_view_keyexpr_t key;
      Check(z_view_keyexpr_from_str(&key, key_.c_str()), "Create Zenoh metadata reply key");
      Owned<z_owned_bytes_t> bytes;
      Check(z_bytes_copy_from_str(&bytes.value, payload.c_str()), "Encode Zenoh metadata reply");
      // The owned query keeps the request alive after the native FIFO callback
      // returns. Dropping it below completes this queryable's response.
      Check(z_query_reply(z_loan(query.value), z_loan(key), z_move(bytes.value), nullptr), "Reply to Zenoh discovery query");
    } catch (...) {
      Owned<z_owned_bytes_t> error;
      if (z_bytes_copy_from_str(&error.value, "Cannot produce Zenoh discovery snapshot.") == Z_OK) {
        z_query_reply_err(z_loan(query.value), z_move(error.value), nullptr);
      }
    }
  }
}

void ZenohDiscoveryResponder::Stop() noexcept {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  // Unblock native FIFO senders before undeclaring. No user C callbacks refer
  // to this object; all responder code has finished when the worker joins.
  z_drop(z_move(queries_.value));
  z_drop(z_move(queryable_.value));
}

void ZenohDiscoveryResponder::Clear() {
  std::lock_guard lock(mutex_);
  endpoints_.clear();
  ++revision_;
}
}  // namespace ibmw::discovery
