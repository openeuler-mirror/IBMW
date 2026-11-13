# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Find ROS2 message packages for message definitions and text introspection.
ibmw_find_target_package(std_msgs QUIET FEATURE ros2-types)
ibmw_find_target_package(geometry_msgs QUIET FEATURE ros2-types)
ibmw_find_target_package(sensor_msgs QUIET FEATURE ros2-types)

if(NOT std_msgs_FOUND)
  message(WARNING "ROS2 std_msgs not found, ibmw_module_ros2_interface will not be built")
  message(WARNING "Please source ROS2 setup.bash: source /opt/ros/humble/setup.bash")
  return()
endif()

message(STATUS "Found ROS2 messages, building ibmw_module_ros2_interface (decoupled mode)")

# In the cross-rosidl slice this interface is used by generated ROS 2 type
# support wrappers.  Keep its DDS ABI tied to the explicit target ROS 2 prefix;
# otherwise an already-loaded source-prefix fastdds target can pull its own
# source-prefix libfastcdr.so transitively into the wrapper link line.
if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL "rosidl")
  ibmw_resolve_target_ros2_library(IBMW_ROS2_TYPES_FASTDDS_TARGET fastrtps)
  ibmw_resolve_target_ros2_library(IBMW_ROS2_TYPES_FASTCDR_TARGET fastcdr)
  set(IBMW_ROS2_TYPES_INCLUDE_DIRS)
  foreach(_ibmw_ros2_types_prefix IN LISTS IBMW_TARGET_ROS2_ROOT IBMW_TARGET_ROS2_DEPS_PREFIX)
    if(_ibmw_ros2_types_prefix AND EXISTS "${_ibmw_ros2_types_prefix}/include")
      list(APPEND IBMW_ROS2_TYPES_INCLUDE_DIRS "$<BUILD_INTERFACE:${_ibmw_ros2_types_prefix}/include>")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES IBMW_ROS2_TYPES_INCLUDE_DIRS)
else()
  if(NOT IBMW_FASTDDS_PROVIDER_TARGET OR NOT IBMW_FASTCDR_PROVIDER_TARGET)
    message(FATAL_ERROR "ros2_types requires the explicitly resolved IBMW DDS provider targets")
  endif()
  set(IBMW_ROS2_TYPES_FASTDDS_TARGET ${IBMW_FASTDDS_PROVIDER_TARGET})
  set(IBMW_ROS2_TYPES_FASTCDR_TARGET ${IBMW_FASTCDR_PROVIDER_TARGET})
endif()
ibmw_cross_sanitize_sysroot_imported_includes(OpenSSL::SSL OpenSSL::Crypto)

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(FILTER head_files EXCLUDE REGEX "/util/")

# Add interface library
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Framework-owned TypeSupport adapter consumed by generated package DSOs.

# Set include path
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)

if(IBMW_ROS2_TYPES_INCLUDE_DIRS)
  target_include_directories(
    ${CUR_TARGET_NAME}
    INTERFACE ${IBMW_ROS2_TYPES_INCLUDE_DIRS})
endif()

# Set head files
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Link ibmw core interfaces
target_link_libraries(
  ${CUR_TARGET_NAME}
  INTERFACE ibmw::api::c
            ibmw::api::cpp
            ibmw::api::ros2_type_support)

# Text serialization instantiates each message's introspection TypeSupport.
# Propagate these libraries to all consumers, including installed targets.
target_link_libraries(
  ${CUR_TARGET_NAME}
  INTERFACE std_msgs::std_msgs__rosidl_generator_cpp
            std_msgs::std_msgs__rosidl_typesupport_introspection_cpp)

if(geometry_msgs_FOUND)
  target_link_libraries(
    ${CUR_TARGET_NAME}
    INTERFACE geometry_msgs::geometry_msgs__rosidl_generator_cpp
              geometry_msgs::geometry_msgs__rosidl_typesupport_introspection_cpp)
  message(STATUS "  - geometry_msgs support enabled")
endif()

if(sensor_msgs_FOUND)
  target_link_libraries(
    ${CUR_TARGET_NAME}
    INTERFACE sensor_msgs::sensor_msgs__rosidl_generator_cpp
              sensor_msgs::sensor_msgs__rosidl_typesupport_introspection_cpp)
  message(STATUS "  - sensor_msgs support enabled")
endif()

# Link FastDDS and FastCDR
target_link_libraries(
  ${CUR_TARGET_NAME}
  INTERFACE ${IBMW_ROS2_TYPES_FASTDDS_TARGET}
            ${IBMW_ROS2_TYPES_FASTCDR_TARGET})

# Build TypeSupport implementation (contains static registration)
add_library(${CUR_TARGET_NAME}_impl
  util/builtin_types.cc
)

target_link_libraries(${CUR_TARGET_NAME}_impl
  PRIVATE ${CUR_TARGET_NAME}
)

add_library(${CUR_TARGET_ALIAS_NAME}_impl ALIAS ${CUR_TARGET_NAME}_impl)

# Set installation
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME} ${CUR_TARGET_NAME}_impl
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
endif()

message(STATUS "ibmw_module_ros2_interface configured successfully (decoupled mode)")
message(STATUS "  - No dependency on rclcpp/rmw_fastrtps_cpp")
message(STATUS "  - Direct FastDDS/FastCDR usage")
message(STATUS "  - ibmw callback execution")

if(IBMW_BUILD_TESTS)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME}
                   TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/util/ros2_text_serialization_test.cc
                            ${CMAKE_CURRENT_SOURCE_DIR}/util/ros2_type_support_test.cc)
  # The CDR size test needs the generated FastRTPS callbacks.
  foreach(_ibmw_ros2_types_test_pkg IN ITEMS std_msgs geometry_msgs sensor_msgs)
    if(TARGET ${_ibmw_ros2_types_test_pkg}::${_ibmw_ros2_types_test_pkg}__rosidl_typesupport_fastrtps_cpp)
      target_link_libraries(${CUR_TARGET_NAME}_test PRIVATE
        ${_ibmw_ros2_types_test_pkg}::${_ibmw_ros2_types_test_pkg}__rosidl_typesupport_fastrtps_cpp)
    endif()
  endforeach()
endif()
