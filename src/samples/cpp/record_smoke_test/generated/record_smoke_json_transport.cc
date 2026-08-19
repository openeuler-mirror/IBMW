// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/record_smoke_transport.h"

#include <memory>
#include <utility>

#include "cpp/util/json_serialization.h"
#include "record_smoke_message.h"

namespace ibmw::examples::cpp::record_smoke_test::generated {

bool RegisterJsonPublisher(ibmw::transport::SenderRef publisher) {
  return ibmw::json::transport::RegisterSendType<JsonSmokeMessage>(publisher);
}

bool RegisterJsonSubscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback) {
  return ibmw::json::transport::OnMessage<JsonSmokeMessage>(
      subscriber,
      [callback = std::move(callback)](
          const std::shared_ptr<const JsonSmokeMessage>& message) {
        callback(message->sequence);
      });
}

void PublishJson(ibmw::transport::SenderRef publisher, uint64_t sequence) {
  JsonSmokeMessage message{
      .sequence = sequence,
      .payload = "record-smoke-" + std::to_string(sequence)};
  ibmw::json::transport::Send(publisher, message);
}

}  // namespace ibmw::examples::cpp::record_smoke_test::generated
