# Minimal A/B/C/S benchmark endpoint.

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

  add_executable(ibmw_abc_dds_endpoint dds_endpoint.cc ros2_latency_artifact.cc)
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

# S is C on the self-developed shm transport: ../shm_send builds the iox_send
# sources against libibmw_shm_transport.so and needs no RouDi.
if(IBMW_BUILD_SHM_EXTENSION AND TARGET shm_send_publisher_app AND
   TARGET shm_send_subscriber_app)
  add_custom_target(ibmw_abc_shm_endpoint DEPENDS
    shm_send_publisher_app shm_send_subscriber_app ibmw_extensions_shm_ext)
  list(APPEND ABC_SMOKE_DEPENDENCIES ibmw_abc_shm_endpoint)
  list(APPEND ABC_SMOKE_LANES "S=shm_send_publisher_app+shm_send_subscriber_app")
endif()

if(ABC_SMOKE_DEPENDENCIES)
  string(JOIN "; " ABC_SMOKE_OUTPUT ${ABC_SMOKE_LANES})
  add_custom_target(ibmw_abc_smoke
    COMMAND ${CMAKE_COMMAND} -E echo "ABC smoke available lanes: ${ABC_SMOKE_OUTPUT}"
    DEPENDS ${ABC_SMOKE_DEPENDENCIES}
    VERBATIM)
endif()

# Without iceoryx the matrix is A/B/S: S takes C's place and nothing needs RouDi.
if(IBMW_BUILD_SHM_EXTENSION AND NOT IBMW_BUILD_ICEORYX_EXTENSION)
  set(ABC_MATRIX_LANES "A/B/S")
  set(ABC_MATRIX_SEND_ENDPOINT ibmw_abc_shm_endpoint)
  set(ABC_MATRIX_SEND_SAMPLE shm_send)
else()
  set(ABC_MATRIX_LANES "A/B/C")
  set(ABC_MATRIX_SEND_ENDPOINT ibmw_abc_iox_endpoint)
  set(ABC_MATRIX_SEND_SAMPLE iox_send)
endif()

set(ABC_MATRIX_TARGET_DEPENDENCIES
  ibmw_abc_ros2_endpoint
  ibmw_abc_dds_endpoint
  ${ABC_MATRIX_SEND_ENDPOINT})
set(ABC_MATRIX_MISSING_TARGETS)
foreach(ABC_MATRIX_DEPENDENCY IN LISTS ABC_MATRIX_TARGET_DEPENDENCIES)
  if(NOT TARGET ${ABC_MATRIX_DEPENDENCY})
    list(APPEND ABC_MATRIX_MISSING_TARGETS ${ABC_MATRIX_DEPENDENCY})
  endif()
endforeach()

set(ABC_MATRIX_ROUDI_COMMAND)
if(ABC_MATRIX_LANES STREQUAL "A/B/C")
  include(IbmwIceoryxRouDi)
  ibmw_resolve_iceoryx_roudi_target(ABC_MATRIX_ROUDI_TARGET)
  if(NOT ABC_MATRIX_ROUDI_TARGET)
    list(APPEND ABC_MATRIX_MISSING_TARGETS
      "iox-roudi or iceoryx_posh::iox-roudi")
  else()
    get_target_property(ABC_MATRIX_ROUDI_IMPORTED
      ${ABC_MATRIX_ROUDI_TARGET} IMPORTED)
    if(NOT ABC_MATRIX_ROUDI_IMPORTED)
      list(APPEND ABC_MATRIX_TARGET_DEPENDENCIES ${ABC_MATRIX_ROUDI_TARGET})
    endif()
    ibmw_generate_iceoryx_roudi_path(
      "${CMAKE_BINARY_DIR}/ibmw_abc_roudi_path.txt")
    set(ABC_MATRIX_ROUDI_COMMAND
      COMMAND ${CMAKE_COMMAND} -E echo
        "RouDi executable: $<TARGET_FILE:${ABC_MATRIX_ROUDI_TARGET}>")
  endif()
endif()

if(NOT ABC_MATRIX_MISSING_TARGETS)
  add_custom_target(ibmw_abc_matrix
    COMMAND ${CMAKE_COMMAND} -E echo
      "ABC matrix is runnable with src/samples/cpp/transport_benchmark/abc_interop/run_block.py"
    ${ABC_MATRIX_ROUDI_COMMAND}
    DEPENDS
      ${ABC_MATRIX_TARGET_DEPENDENCIES}
      ${ABC_DIR}/run_block.py
      ${ABC_DIR}/config/publisher.yaml
      ${ABC_DIR}/config/subscriber.yaml
      ${ABC_DIR}/../${ABC_MATRIX_SEND_SAMPLE}/config/linux/bin/cfg/${ABC_MATRIX_SEND_SAMPLE}/publisher.yaml
      ${ABC_DIR}/../${ABC_MATRIX_SEND_SAMPLE}/config/linux/bin/cfg/${ABC_MATRIX_SEND_SAMPLE}/subscriber.yaml
    VERBATIM)
  message(STATUS "${ABC_MATRIX_LANES} benchmark matrix target available: ibmw_abc_matrix")
else()
  string(JOIN ", " ABC_MATRIX_MISSING_OUTPUT ${ABC_MATRIX_MISSING_TARGETS})
  message(STATUS
    "${ABC_MATRIX_LANES} benchmark matrix target unavailable; missing targets: ${ABC_MATRIX_MISSING_OUTPUT}. "
    "ibmw_abc_smoke remains available for configured lanes.")
endif()
