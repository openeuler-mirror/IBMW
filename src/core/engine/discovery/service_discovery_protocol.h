// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <mutex>
#include "extension_api/ibmw_service_discovery.h"

namespace ibmw::discovery::service_protocol {

inline constexpr char kHttpPath[] = "/ibmw/discovery/services/v1";
inline constexpr char kGrpcPath[] = "/ibmw.discovery.v1.ServiceDirectory/List";
inline constexpr char kMqttRequestTopic[] = "ibmw/discovery/services/v1/request";
inline constexpr char kMqttReplyPrefix[] = "ibmw/discovery/services/v1/reply/";
inline constexpr size_t kMaxSnapshotBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxServices = 10000;

std::string NewInstanceId();
bool IsHidden(std::string_view name);
std::string ZenohPrefix(std::string_view domain);

// Only public service identities/types travel over the network. Registry
// filesystem paths, control sockets and library/package paths stay local.
class Catalog {
 public:
  void Add(std::string_view name, std::string_view request, std::string_view response);
  std::string Encode() const;
  const std::string& Instance() const noexcept { return instance_; }
 private:
  std::string instance_ = NewInstanceId();
  mutable std::mutex mutex_;
  std::map<std::string, ibmw::extension::ServiceAdvertisement> services_;
};

ServiceDiscoverySnapshot Decode(std::string_view payload, const ServiceDiscoverySource& source);

struct ServerAddress { std::string host; std::string port; };
ServerAddress ParseServerAddress(std::string_view endpoint, std::string_view scheme);
std::string GrpcFrame(std::string_view payload);
std::string_view GrpcPayload(std::string_view frame);

}  // namespace ibmw::discovery::service_protocol
