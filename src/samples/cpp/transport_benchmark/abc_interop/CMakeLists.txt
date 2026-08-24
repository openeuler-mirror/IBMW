# Minimal A/B/C benchmark endpoint.

set(ABC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ABC_SMOKE_DEPENDENCIES)
set(ABC_SMOKE_LANES)

if(IBMW_BUILD_WITH_ROS2)
  find_package(rclcpp QUIET)
  find_package(std_msgs QUIET)
  find_package(rosidl_typesupport_cpp QUIET)
  find_package(rmw_fastrtps_cpp QUIET)
  if(rclcpp_FOUND AND std_msgs_FOUND)
    add_executable(ibmw_abc_ros2_endpoint ros2_endpoint.cc ros2_latency_artifact.cc)
    target_link_libraries(ibmw_abc_ros2_endpoint PRIVATE
      ibmw::base rclcpp::rclcpp std_msgs::std_msgs__rosidl_generator_cpp
      std_msgs::std_msgs__rosidl_typesupport_cpp)
    if(TARGET std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
      target_link_libraries(ibmw_abc_ros2_endpoint PRIVATE
        std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
    endif()
    target_compile_features(ibmw_abc_ros2_endpoint PRIVATE cxx_std_20)
    target_include_directories(ibmw_abc_ros2_endpoint PRIVATE
      ${CMAKE_SOURCE_DIR}/src)
    list(APPEND ABC_SMOKE_DEPENDENCIES ibmw_abc_ros2_endpoint)
    list(APPEND ABC_SMOKE_LANES "A=ibmw_abc_ros2_endpoint")
  else()
    message(WARNING "Skipping A endpoint: rclcpp and std_msgs are required")
  endif()
endif()

if(IBMW_BUILD_TESTS)
  add_executable(ibmw_abc_ros2_latency_artifact_test
    ros2_latency_artifact_test.cc ros2_latency_artifact.cc)
  target_compile_features(ibmw_abc_ros2_latency_artifact_test PRIVATE cxx_std_20)
  add_test(NAME ibmw_abc_ros2_latency_artifact_test COMMAND ibmw_abc_ros2_latency_artifact_test)
endif()

# B uses IBMW's typed ROS2 message adapter over the DDS extension. Its serializer
# is private to this benchmark and does not participate in the global registry.
if(IBMW_BUILD_ROS2_TYPES AND IBMW_BUILD_DDS_EXTENSION AND
   TARGET ibmw::extensions::ros2_types AND TARGET ibmw_dds_extension)
  add_library(ibmw_abc_uint8_multi_array_serializer STATIC
    uint8_multi_array_serializer.cc)
  target_compile_features(ibmw_abc_uint8_multi_array_serializer PRIVATE cxx_std_20)
  target_include_directories(ibmw_abc_uint8_multi_array_serializer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ibmw_abc_uint8_multi_array_serializer PRIVATE
    ibmw::extensions::ros2_types)

  add_executable(ibmw_abc_dds_endpoint dds_endpoint.cc)
  target_compile_features(ibmw_abc_dds_endpoint PRIVATE cxx_std_20)
  target_include_directories(ibmw_abc_dds_endpoint PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ibmw_abc_dds_endpoint PRIVATE
    ibmw::core::engine ibmw_dds_extension ibmw::extensions::ros2_types
    ibmw_abc_uint8_multi_array_serializer)

  if(IBMW_BUILD_TESTS)
    add_executable(ibmw_abc_uint8_multi_array_serializer_test
      uint8_multi_array_serializer_test.cc)
    target_compile_features(ibmw_abc_uint8_multi_array_serializer_test PRIVATE cxx_std_20)
    target_include_directories(ibmw_abc_uint8_multi_array_serializer_test PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ibmw_abc_uint8_multi_array_serializer_test PRIVATE
      ibmw_abc_uint8_multi_array_serializer ibmw::extensions::ros2_types
      GTest::gtest GTest::gtest_main)
    if(TARGET std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
      target_link_libraries(ibmw_abc_uint8_multi_array_serializer_test PRIVATE
        std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
      target_compile_definitions(ibmw_abc_uint8_multi_array_serializer_test PRIVATE
        IBMW_ABC_HAS_STD_MSGS_FASTRTPS_CPP=1)
    endif()
    add_test(NAME ibmw_abc_uint8_multi_array_serializer_test
      COMMAND ibmw_abc_uint8_multi_array_serializer_test)
    if(TARGET std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
      set_tests_properties(ibmw_abc_uint8_multi_array_serializer_test PROPERTIES
        ENVIRONMENT_MODIFICATION
          "LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp>")
    endif()
  endif()
  add_custom_command(TARGET ibmw_abc_dds_endpoint POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/config ${CMAKE_BINARY_DIR}/cfg/abc_interop)
  list(APPEND ABC_SMOKE_DEPENDENCIES ibmw_abc_dds_endpoint)
  list(APPEND ABC_SMOKE_LANES "B=ibmw_abc_dds_endpoint")
endif()

# C is the existing, tested iox_send ordinary Send() path.  Its targets are
# added by ../iox_send under the ICEORYX_EXTENSION gate; no duplicate ABI or
# transport implementation is introduced here.
if(IBMW_BUILD_ICEORYX_EXTENSION AND TARGET iox_send_publisher_app AND
   TARGET iox_send_subscriber_app)
  add_custom_target(ibmw_abc_iox_endpoint DEPENDS
    iox_send_publisher_app iox_send_subscriber_app)
  list(APPEND ABC_SMOKE_DEPENDENCIES ibmw_abc_iox_endpoint)
  list(APPEND ABC_SMOKE_LANES "C=iox_send_publisher_app+iox_send_subscriber_app")
endif()

if(ABC_SMOKE_DEPENDENCIES)
  string(JOIN "; " ABC_SMOKE_OUTPUT ${ABC_SMOKE_LANES})
  add_custom_target(ibmw_abc_smoke
    COMMAND ${CMAKE_COMMAND} -E echo "ABC smoke available lanes: ${ABC_SMOKE_OUTPUT}"
    DEPENDS ${ABC_SMOKE_DEPENDENCIES}
    VERBATIM)
endif()
