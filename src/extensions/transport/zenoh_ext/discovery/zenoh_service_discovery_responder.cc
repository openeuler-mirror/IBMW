// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "zenoh_service_discovery_responder.h"

namespace ibmw::discovery {
using zenoh_detail::Check;
using zenoh_detail::Owned;
void ZenohServiceDiscoveryResponder::Start(const z_loaned_session_t* session, std::string_view domain) {
  if (worker_.joinable()) throw std::logic_error("Zenoh service directory already started.");
  key_ = service_protocol::ZenohPrefix(domain) + catalog_.Instance();
  static_cast<void>(catalog_.Encode());
  z_view_keyexpr_t key;
  Check(z_view_keyexpr_from_str(&key, key_.c_str()), "Create Zenoh service directory key");
  Owned<z_owned_closure_query_t> callback;
  z_fifo_channel_query_new(&callback.value, &queries_.value, 16);
  Check(z_declare_queryable(session, &queryable_.value, z_loan(key), z_move(callback.value), nullptr),
        "Declare Zenoh service directory");
  worker_ = std::jthread([this](std::stop_token stop) { Serve(stop); });
}
void ZenohServiceDiscoveryResponder::Serve(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Owned<z_owned_query_t> query;
    const auto code = z_fifo_handler_query_try_recv(z_loan(queries_.value), &query.value);
    if (code == Z_CHANNEL_DISCONNECTED) return;
    if (code != Z_OK) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    try {
      const auto payload = catalog_.Encode();
      z_view_keyexpr_t key;
      Check(z_view_keyexpr_from_str(&key, key_.c_str()), "Create Zenoh reply key");
      Owned<z_owned_bytes_t> bytes;
      Check(z_bytes_copy_from_str(&bytes.value, payload.c_str()), "Encode Zenoh service directory");
      Check(z_query_reply(z_loan(query.value), z_loan(key), z_move(bytes.value), nullptr), "Reply to service directory query");
    } catch (...) {
      Owned<z_owned_bytes_t> error;
      if (z_bytes_copy_from_str(&error.value, "Cannot produce service directory.") == Z_OK)
        z_query_reply_err(z_loan(query.value), z_move(error.value), nullptr);
    }
  }
}
void ZenohServiceDiscoveryResponder::Stop() noexcept {
  if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
  z_drop(z_move(queries_.value));
  z_drop(z_move(queryable_.value));
}
}  // namespace ibmw::discovery
