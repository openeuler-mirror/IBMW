// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/record_smoke_transport.h"

#include <memory>
#include <utility>

#include "example_ros2/msg/ros_test_msg.hpp"
#include "type_support/ros2/ros2_transport.h"
#include "rosgraph_msgs/msg/clock.hpp"

namespace ibmw::examples::cpp::record_smoke_test::generated {

using RosTestMsg = example_ros2::msg::RosTestMsg;

bool RegisterRos2Publisher(ibmw::transport::SenderRef publisher) {
  return ibmw::ros2::transport::RegisterSendType<RosTestMsg>(publisher);
}

bool RegisterRos2Subscriber(
    ibmw::transport::ReceiverRef subscriber,
    SequenceCallback callback) {
  return ibmw::ros2::transport::OnMessage<RosTestMsg>(
      subscriber,
      [callback = std::move(callback)](
          const std::shared_ptr<const RosTestMsg>& message) {
        callback(static_cast<uint64_t>(message->num));
      });
}

void PublishRos2(ibmw::transport::SenderRef publisher, uint64_t sequence) {
  RosTestMsg message;
  message.num = static_cast<int64_t>(sequence);
  message.data = {1, 2, 3, 4};
  message.test_msg2.num = static_cast<int64_t>(sequence);
  message.test_msg2.num2 = 1.0;
  message.test_msg2.data = 1;
  ibmw::ros2::transport::Send(publisher, message);
}

bool RegisterClockPublisher(ibmw::transport::SenderRef publisher) {
  return ibmw::ros2::transport::RegisterSendType<rosgraph_msgs::msg::Clock>(
      publisher);
}

void PublishClock(ibmw::transport::SenderRef publisher, uint64_t timestamp_ns) {
  rosgraph_msgs::msg::Clock message;
  message.clock.sec = static_cast<int32_t>(timestamp_ns / 1000000000ull);
  message.clock.nanosec =
      static_cast<uint32_t>(timestamp_ns % 1000000000ull);
  ibmw::ros2::transport::Send(publisher, message);
}

}  // namespace ibmw::examples::cpp::record_smoke_test::generated
