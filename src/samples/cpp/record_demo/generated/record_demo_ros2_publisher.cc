// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/record_demo_publishers.h"

#include "example_ros2/msg/ros_test_msg.hpp"
#include "type_support/ros2/ros2_transport.h"

namespace ibmw::examples::cpp::record_demo::generated {

using RosTestMsg = example_ros2::msg::RosTestMsg;

bool Ros2Publisher::Initialize(ibmw::transport::SenderRef publisher) {
  publisher_ = publisher;
  return ibmw::ros2::transport::RegisterSendType<RosTestMsg>(publisher_);
}

void Ros2Publisher::Publish(uint64_t sequence) {
  RosTestMsg message;
  message.num = static_cast<int64_t>(sequence);
  message.data = {1, 2, 3, 4};
  message.test_msg2.num = 0;
  message.test_msg2.num2 = 0.0;
  message.test_msg2.data = 0;
  ibmw::ros2::transport::Send(publisher_, message);
}

}  // namespace ibmw::examples::cpp::record_demo::generated
