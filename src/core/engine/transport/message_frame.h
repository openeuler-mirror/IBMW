// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

#include "cpp/transport/transport_context.h"
#include "cpp/util/type_support.h"

namespace ibmw::runtime::core::transport {

/**
 * @brief Topic base info
 *
 */
struct TransportInfo {
  std::string msg_type;
  std::string topic_name;
  std::string pkg_path;
  std::string module_name;
  uint64_t index = 0;

  ibmw::util::TypeSupportRef msg_type_support_ref;
};

/**
 * @brief Sub/Pub msg info with cache
 *
 */
struct MessageFrame {
  /// Topic base info
  const TransportInfo& info;

  /// pointer to msg, may be nullptr
  const void* msg_ptr = nullptr;

  /// ref to ctx, may be empty
  ibmw::transport::ContextRef ctx_ref;

  /// serialization cache
  std::unordered_map<
      std::string,
      std::shared_ptr<ibmw::util::BufferArrayView>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      serialization_cache;

  /// msg cache
  std::shared_ptr<void> msg_cache_ptr;
};

}  // namespace ibmw::runtime::core::transport
