# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

cmake_minimum_required(VERSION 3.24)
project(ibmw_text_typesupport_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(ibmw REQUIRED CONFIG)
include(CTest)
include(DdsIdlGenCode)
add_dds_idl_gencode_target(TARGET_NAME text_messages
  IDL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Inheritance.idl" "${CMAKE_CURRENT_SOURCE_DIR}/Names.idl"
  GENCODE_PATH "${CMAKE_CURRENT_BINARY_DIR}/gencode")
add_executable(text_typesupport text_typesupport.cc)
target_link_libraries(text_typesupport PRIVATE text_messages)
add_test(NAME text_typesupport COMMAND text_typesupport)
option(IBMW_TEST_ROS2_SERVICE "Test imported ROS2 service TypeSupport" OFF)
if(IBMW_TEST_ROS2_SERVICE)
  find_package(rosidl_typesupport_cpp REQUIRED)
  ibmw_add_rosidl_interface_target(service_iface PACKAGE_NAME example_interfaces
    DEPS example_interfaces::example_interfaces__rosidl_typesupport_cpp
         rosidl_typesupport_cpp::rosidl_typesupport_cpp)
  add_executable(ros_service ros_service.cc)
  target_link_libraries(ros_service PRIVATE service_iface ibmw::api::ros2_type_support)
  add_test(NAME ros_service COMMAND ros_service)
endif()
