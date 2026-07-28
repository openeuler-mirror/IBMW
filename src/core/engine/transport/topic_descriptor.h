// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "cpp/util/schema_support.h"

namespace ibmw::runtime::core::transport {

struct QosDescriptor {
  std::map<std::string, std::string> policies;

  bool Empty() const noexcept { return policies.empty(); }
};

struct TopicDescriptor {
  std::string topic;
  std::string msg_type;
  std::string serialization_type;
  ibmw::util::SchemaDescriptor schema;
  QosDescriptor qos;
  std::vector<std::string> available_drivers;
};

}  // namespace ibmw::runtime::core::transport
