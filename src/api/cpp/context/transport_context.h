// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <any>

#include "cpp/transport/transport_context.h"
#include "cpp/transport/transport_handle.h"

namespace ibmw::context {

enum class TransportState : uint32_t {
  kOn = 0,
  kOff,
};

struct TransportResource {
  ibmw::transport::SenderRef pub;
  ibmw::transport::ReceiverRef sub;
  std::any pub_f;
  std::any sub_f;
};

}  // namespace ibmw::context
