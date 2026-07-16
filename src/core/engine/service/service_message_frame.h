// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cpp/service/rpc_context.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/type_support.h"

namespace ibmw::runtime::core::service {

struct ServiceInfo {
  std::string func_name;
  std::string pkg_path;
  std::string module_name;
  uint64_t index = 0;

  const void* custom_type_support_ptr;
  ibmw::util::TypeSupportRef req_type_support_ref;
  ibmw::util::TypeSupportRef rsp_type_support_ref;
};

struct ServiceMessageFrame {
  const ServiceInfo& info;

  const void* req_ptr;
  void* rsp_ptr;

  ibmw::rpc::ContextRef ctx_ref;

  std::function<void(ibmw::rpc::Status)> callback;

  std::unordered_map<
      std::string,
      std::shared_ptr<ibmw::util::BufferArrayView>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      req_serialization_cache;
  std::unordered_map<
      std::string,
      std::shared_ptr<ibmw::util::BufferArrayView>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      rsp_serialization_cache;
};

}  // namespace ibmw::runtime::core::service
