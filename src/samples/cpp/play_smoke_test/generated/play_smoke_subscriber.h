// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cpp/transport/transport_handle.h"

namespace ibmw::examples::cpp::play_smoke_test::generated {

struct ReceivedMessage {
  uint64_t sequence = 0;
  std::string payload;
  std::string serialization;
  std::string driver;
};

using MessageCallback = std::function<void(ReceivedMessage)>;

bool RegisterCdrSubscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback);
bool RegisterJsonSubscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback);
bool RegisterRos2Subscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback);

}  // namespace ibmw::examples::cpp::play_smoke_test::generated
