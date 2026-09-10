// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>

#include "cpp/transport/transport_handle.h"

namespace ibmw::examples::cpp::record_smoke_test::generated {

using SequenceCallback = std::function<void(uint64_t)>;

bool RegisterCdrPublisher(ibmw::transport::SenderRef publisher);
bool RegisterCdrSubscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback);
void PublishCdr(ibmw::transport::SenderRef publisher, uint64_t sequence);

bool RegisterRos2Publisher(ibmw::transport::SenderRef publisher);
bool RegisterRos2Subscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback);
void PublishRos2(ibmw::transport::SenderRef publisher, uint64_t sequence);

bool RegisterClockPublisher(ibmw::transport::SenderRef publisher);
void PublishClock(ibmw::transport::SenderRef publisher, uint64_t timestamp_ns);

bool RegisterJsonPublisher(ibmw::transport::SenderRef publisher);
bool RegisterJsonSubscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback);
void PublishJson(ibmw::transport::SenderRef publisher, uint64_t sequence);

}  // namespace ibmw::examples::cpp::record_smoke_test::generated
