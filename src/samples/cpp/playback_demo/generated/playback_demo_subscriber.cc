// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/playback_demo_subscriber.h"

#include <memory>
#include <utility>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "type_support/dds/dds_transport.h"

IBMW_DDS_TYPE_NAME(
    ibmw::protocols::common::Header,
    "ibmw::protocols::common::Header")

namespace ibmw::examples::cpp::playback_demo::generated {

using Header = ibmw::protocols::common::Header;

bool PlaybackSubscriber::Subscribe(
    ibmw::transport::ReceiverRef subscriber, Callback callback) {
  return ibmw::transport::OnMessage<Header>(
      subscriber,
      [callback{std::move(callback)}](
          const std::shared_ptr<const Header>& message) {
        const PlaybackMessage playback_message{
            .sequence = message->sequence_num(),
            .timestamp = message->time_stamp(),
            .frame_id = message->frame_id()};
        callback(playback_message);
      });
}

}  // namespace ibmw::examples::cpp::playback_demo::generated
