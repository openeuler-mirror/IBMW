// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/record_demo_publishers.h"

#include <chrono>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "type_support/dds/dds_transport.h"

IBMW_DDS_TYPE_NAME(
    ibmw::protocols::common::Header,
    "ibmw::protocols::common::Header")

namespace ibmw::examples::cpp::record_demo::generated {

using Header = ibmw::protocols::common::Header;

bool CdrPublisher::Initialize(ibmw::transport::SenderRef publisher) {
  publisher_ = publisher;
  return ibmw::transport::RegisterSendType<Header>(publisher_);
}

void CdrPublisher::Publish(uint64_t sequence) {
  Header message;
  message.time_stamp(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count()));
  message.frame_id("record_demo");
  message.sequence_num(sequence);
  ibmw::transport::Send(publisher_, message);
}

}  // namespace ibmw::examples::cpp::record_demo::generated
