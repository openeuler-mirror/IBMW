// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "example_interfaces/srv/add_two_ints.hpp"
#include "type_support/ros2/ros2_type_support_factory.h"
#include "type_support/type_support_pkg_main.h"
extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_base_t* entries[] = {
      ibmw::ros2::GetIbmwTypeSupport<example_interfaces::srv::AddTwoInts_Request>("example_interfaces/srv/AddTwoInts_Request"),
      ibmw::ros2::GetIbmwTypeSupport<example_interfaces::srv::AddTwoInts_Response>("example_interfaces/srv/AddTwoInts_Response")};
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2,
      .struct_size = sizeof(ibmw_type_support_package_v1_t), .entry_count = 2, .entries = entries};
  return &package;
}
