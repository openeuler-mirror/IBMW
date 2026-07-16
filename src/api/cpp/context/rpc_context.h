// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <any>
#include "cpp/service/rpc_context.h"
#include "cpp/util/function.h"
#include "c/service/rpc_handle_base.h"

namespace ibmw::context {

enum class RpcState : uint32_t {
  kOn = 0,
  kOff,
};

struct RpcResource {
  std::any call_f;
  std::any serve_f;
  std::string func_name;
};

}  // namespace ibmw::context
