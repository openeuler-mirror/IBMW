// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <mutex>
#include <thread>

#include "zenoh_discovery_util.h"

namespace ibmw::discovery {

// Owned by the business session. Only successfully declared transport
// endpoints enter this catalog; RPC internals are not topic advertisements.
class ZenohDiscoveryResponder {
 public:
  ~ZenohDiscoveryResponder() { Stop(); }
  void Record(EndpointRole role, std::string topic, std::string type, std::string key);
  void Start(const z_loaned_session_t* session, std::string domain);
  void Stop() noexcept;
  void Clear();

 private:
  zenoh_protocol::Snapshot Snapshot() const;
  void Serve();

  mutable std::mutex mutex_;
  std::map<std::pair<EndpointRole, std::string>, DiscoveredEndpoint> endpoints_;
  uint64_t revision_ = 0;
  std::string instance_;
  std::string domain_;
  std::string key_;
  zenoh_detail::Owned<z_owned_queryable_t> queryable_;
  zenoh_detail::Owned<z_owned_ring_handler_query_t> queries_;
  std::jthread worker_;
};

}  // namespace ibmw::discovery
