# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_library(ibmw_discovery_ros2 SHARED ros2_discovery_provider.cc)
target_link_libraries(ibmw_discovery_ros2 PRIVATE ibmw::core::extension_api rclcpp::rclcpp)
set_target_properties(ibmw_discovery_ros2 PROPERTIES
  OUTPUT_NAME ibmw_discovery_ros2 LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
add_werror(ibmw_discovery_ros2)
if(IBMW_INSTALL)
  install(TARGETS ibmw_discovery_ros2 LIBRARY DESTINATION bin COMPONENT discovery)
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_discovery_ros2_test ros2_discovery_provider_test.cc)
  target_link_libraries(ibmw_discovery_ros2_test PRIVATE ibmw_discovery_ros2
    ibmw::core::extension_api rclcpp::rclcpp GTest::gtest_main)
  add_werror(ibmw_discovery_ros2_test)
  add_test(NAME ibmw_discovery_ros2_test COMMAND ibmw_discovery_ros2_test)
  set_tests_properties(ibmw_discovery_ros2_test PROPERTIES TIMEOUT 30
    LABELS "integration;discovery;ros2"
    ENVIRONMENT "ROS_LOCALHOST_ONLY=1;ROS_DOMAIN_ID=79")
endif()
