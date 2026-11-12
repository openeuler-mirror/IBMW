// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <thread>
#include "engine/discovery/service_discovery_protocol.h"
#include "zenoh_discovery_util.h"

namespace ibmw::discovery {
class ZenohServiceDiscoveryResponder {
 public:
  ~ZenohServiceDiscoveryResponder() { Stop(); }
  service_protocol::Catalog& Catalog() noexcept { return catalog_; }
  void Start(const z_loaned_session_t* session, std::string_view domain);
  void Stop() noexcept;
 private:
  void Serve(std::stop_token stop);
  service_protocol::Catalog catalog_;
  std::string key_;
  zenoh_detail::Owned<z_owned_queryable_t> queryable_;
  zenoh_detail::Owned<z_owned_fifo_handler_query_t> queries_;
  std::jthread worker_;
};
}  // namespace ibmw::discovery
