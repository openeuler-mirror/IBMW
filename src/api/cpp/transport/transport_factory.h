// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/core.h"
#include "cpp/transport/transport_handle.h"

// Public factory header: stays free of transport-extension implementation
// includes.
// SenderAccess<T> / ReceiverAccess<T> are forward-declared in
// transport_handle.h; the typesupport header that supplies the specialization
// for a given T (e.g. type_support/dds/dds_transport.h for DDS message
// types) is included by the consumer TU. This keeps DDS-only and local-only
// modules free of transitive rosidl/json dependencies.

namespace ibmw::transport {

template <typename T>
inline SenderAccess<T> publisher_for(Runtime& rt, std::string_view topic) {
  return SenderAccess<T>(rt.GetTransport().GetPublisher(topic));
}

template <typename T>
inline ReceiverAccess<T> subscriber_for(Runtime& rt, std::string_view topic) {
  return ReceiverAccess<T>(rt.GetTransport().GetSubscriber(topic));
}

}  // namespace ibmw::transport
