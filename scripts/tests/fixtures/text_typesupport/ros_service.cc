// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <stdexcept>
#include "example_interfaces/srv/add_two_ints.hpp"
#include "type_support/ros2/ros2_type_support_factory.h"

int main() {
  const auto* support = ibmw::GetRos2MessageTypeSupport<example_interfaces::srv::AddTwoInts::Request>();
  if (!support) throw std::runtime_error("missing imported service TypeSupport");
}
