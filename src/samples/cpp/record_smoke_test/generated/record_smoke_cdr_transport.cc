// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/record_smoke_transport.h"

#include <chrono>
#include <memory>
#include <utility>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "type_support/dds/dds_transport.h"

IBMW_DDS_TYPE_NAME(
    ibmw::protocols::common::Header,
    "ibmw::protocols::common::Header")

namespace ibmw::examples::cpp::record_smoke_test::generated {

using Header = ibmw::protocols::common::Header;

bool RegisterCdrPublisher(ibmw::transport::SenderRef publisher) {
  return ibmw::transport::RegisterSendType<Header>(publisher);
}

bool RegisterCdrSubscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback) {
  return ibmw::transport::OnMessage<Header>(
      subscriber,
      [callback = std::move(callback)](
          const std::shared_ptr<const Header>& message) {
        callback(message->sequence_num());
      });
}

void PublishCdr(ibmw::transport::SenderRef publisher, uint64_t sequence) {
  Header message;
  message.time_stamp(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count()));
  message.frame_id("record_smoke");
  message.sequence_num(sequence);
  ibmw::transport::Send(publisher, message);
}

}  // namespace ibmw::examples::cpp::record_smoke_test::generated
