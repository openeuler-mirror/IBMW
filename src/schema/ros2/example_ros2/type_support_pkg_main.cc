// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include "ros2_types/util/ros2_type_support_factory.h"

#include "example_ros2/msg/benchmark_message.hpp"
#include "example_ros2/msg/benchmark_signal.hpp"
#include "example_ros2/msg/benchmark_status.hpp"
#include "example_ros2/msg/ros_test_data.hpp"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "example_ros2/srv/ros_test_rpc.hpp"

static const ibmw_type_support_base_t* type_support_array[]{
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::RosTestMsg>(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::RosTestData>(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::BenchmarkMessage>(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::BenchmarkSignal>(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::BenchmarkStatus>(),
};

extern "C" {

size_t IbmwDynlibGetTypeSupportArrayLength() {
  return sizeof(type_support_array) / sizeof(type_support_array[0]);
}

const ibmw_type_support_base_t** IbmwDynlibGetTypeSupportArray() {
  return type_support_array;
}
}
