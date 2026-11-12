// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "std_msgs/msg/int32.hpp"
#include "example_interfaces/srv/add_two_ints.hpp"
#include "type_support/ros2/ros2_type_support_factory.h"
#include "type_support/type_support_pkg_main.h"

extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_base_t* entries[] = {
      ibmw::GetRos2MessageTypeSupport<std_msgs::msg::Int32>(),
      ibmw::GetRos2MessageTypeSupport<example_interfaces::srv::AddTwoInts_Request>(),
      ibmw::GetRos2MessageTypeSupport<example_interfaces::srv::AddTwoInts_Response>()};
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = 3, .entries = entries};
  return &package;
}
