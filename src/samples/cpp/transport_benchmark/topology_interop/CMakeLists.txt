set(TOPOLOGY_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Keep the directory safe to include from both the parent benchmark path and
# the legacy interop path used by older build graphs.
if(TARGET ibmw_topology_dds_endpoint)
  return()
endif()

if(IBMW_BUILD_WITH_ROS2)
  find_package(rclcpp QUIET)
  find_package(std_msgs QUIET)
  if(rclcpp_FOUND AND std_msgs_FOUND)
    add_executable(ibmw_topology_ros2_endpoint ros2_endpoint.cc topology_artifact.cc)
    target_compile_features(ibmw_topology_ros2_endpoint PRIVATE cxx_std_20)
    target_include_directories(ibmw_topology_ros2_endpoint PRIVATE ${CMAKE_SOURCE_DIR}/src ${TOPOLOGY_DIR})
    target_link_libraries(ibmw_topology_ros2_endpoint PRIVATE ibmw::base rclcpp::rclcpp
      std_msgs::std_msgs__rosidl_generator_cpp std_msgs::std_msgs__rosidl_typesupport_cpp)
    if(TARGET std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
      target_link_libraries(ibmw_topology_ros2_endpoint PRIVATE std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
    endif()
  endif()
endif()

if(IBMW_BUILD_ROS2_TYPES AND IBMW_BUILD_DDS_EXTENSION AND
   TARGET ibmw::extensions::ros2_types AND TARGET ibmw_dds_extension)
  add_library(ibmw_topology_uint8_serializer STATIC ../abc_interop/uint8_multi_array_serializer.cc)
  target_compile_features(ibmw_topology_uint8_serializer PRIVATE cxx_std_20)
  target_include_directories(ibmw_topology_uint8_serializer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../abc_interop)
  target_link_libraries(ibmw_topology_uint8_serializer PRIVATE ibmw::extensions::ros2_types)
  add_executable(ibmw_topology_dds_endpoint dds_endpoint.cc topology_artifact.cc)
  target_compile_features(ibmw_topology_dds_endpoint PRIVATE cxx_std_20)
  target_include_directories(ibmw_topology_dds_endpoint PRIVATE ${CMAKE_SOURCE_DIR}/src ${TOPOLOGY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../abc_interop)
  target_link_libraries(ibmw_topology_dds_endpoint PRIVATE ibmw::core::engine ibmw_dds_extension
    ibmw::extensions::ros2_types ibmw_topology_uint8_serializer)
  add_custom_command(TARGET ibmw_topology_dds_endpoint POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${TOPOLOGY_DIR}/config ${CMAKE_BINARY_DIR}/cfg/topology_interop)
endif()
