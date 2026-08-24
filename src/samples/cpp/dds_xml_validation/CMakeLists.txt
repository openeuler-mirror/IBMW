# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "IBMW DDS XML validation is a cross-compilation-only sample")
endif()
if(NOT IBMW_DDS_PROVIDER STREQUAL "ROS2_MINIMAL_SDK")
  message(FATAL_ERROR "IBMW DDS XML validation requires IBMW_DDS_PROVIDER=ROS2_MINIMAL_SDK")
endif()
if(NOT IBMW_BUILD_ROS2_TYPES)
  message(FATAL_ERROR "IBMW DDS XML validation requires IBMW_BUILD_ROS2_TYPES=ON")
endif()
if(NOT IBMW_ROS2_MINIMAL_SDK_PREFIX OR
   NOT IS_DIRECTORY "${IBMW_ROS2_MINIMAL_SDK_PREFIX}")
  message(FATAL_ERROR "IBMW DDS XML validation requires a valid IBMW_ROS2_MINIMAL_SDK_PREFIX")
endif()

# Build only the existing dds_send_shm two-process path. This target is kept
# separate from the full benchmark matrix so it can be cross-compiled without
# pulling in iceoryx or unrelated Linux-only samples.
set(_DDS_SEND_SHM_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/../transport_benchmark/dds_send_shm)
set(_BENCHMARK_DIR
  ${CMAKE_CURRENT_SOURCE_DIR}/../transport_benchmark)

add_library(ibmw_dds_xml_validation_publisher STATIC
  ${_DDS_SEND_SHM_DIR}/managed/publisher_module/publisher_module.cc)
target_include_directories(ibmw_dds_xml_validation_publisher PUBLIC
  ${_DDS_SEND_SHM_DIR}/managed
  ${_BENCHMARK_DIR}/bench_common
  ${_BENCHMARK_DIR}/bench_payload
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)
target_link_libraries(ibmw_dds_xml_validation_publisher PUBLIC
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::base
  ibmw::extensions::dds_types
  yaml-cpp::yaml-cpp)

add_library(ibmw_dds_xml_validation_subscriber STATIC
  ${_DDS_SEND_SHM_DIR}/managed/subscriber_module/subscriber_module.cc)
target_include_directories(ibmw_dds_xml_validation_subscriber PUBLIC
  ${_DDS_SEND_SHM_DIR}/managed
  ${_BENCHMARK_DIR}/bench_common
  ${_BENCHMARK_DIR}/bench_payload
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)
target_link_libraries(ibmw_dds_xml_validation_subscriber PUBLIC
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::base
  ibmw::extensions::dds_types
  yaml-cpp::yaml-cpp)

add_executable(ibmw_dds_xml_publisher_app
  ${_DDS_SEND_SHM_DIR}/standalone/publisher_app/main.cc)
target_link_libraries(ibmw_dds_xml_publisher_app PRIVATE
  ibmw::core::engine
  ibmw_dds_extension
  ibmw_dds_xml_validation_publisher)

add_executable(ibmw_dds_xml_subscriber_app
  ${_DDS_SEND_SHM_DIR}/standalone/subscriber_app/main.cc)
target_link_libraries(ibmw_dds_xml_subscriber_app PRIVATE
  ibmw::core::engine
  ibmw_dds_extension
  ibmw_dds_xml_validation_subscriber)

set(_IBMW_XML_WIDE_PREFIXES "${IBMW_ROS2_MINIMAL_SDK_PREFIX}")
if(IBMW_TARGET_ROS2_DEPS_PREFIX)
  list(APPEND _IBMW_XML_WIDE_PREFIXES ${IBMW_TARGET_ROS2_DEPS_PREFIX})
endif()
foreach(_pkg IN ITEMS sensor_msgs nav_msgs tf2_msgs visualization_msgs diagnostic_msgs slam_toolbox)
  find_package(${_pkg} REQUIRED
    PATHS ${_IBMW_XML_WIDE_PREFIXES}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)
  set(_ts_target "${_pkg}::${_pkg}__rosidl_typesupport_fastrtps_cpp")
  if(NOT TARGET ${_ts_target})
    message(FATAL_ERROR "${_pkg} did not export required target ${_ts_target}")
  endif()
endforeach()
if(NOT TARGET slam_toolbox::slam_toolbox__rosidl_typesupport_cpp)
  message(FATAL_ERROR
    "slam_toolbox did not export required target slam_toolbox::slam_toolbox__rosidl_typesupport_cpp")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_IBMW_XML_WIDE_SRV
  "${IBMW_ROS2_MINIMAL_SDK_PREFIX}/share/slam_toolbox/srv/SerializePoseGraph.srv")
if(NOT EXISTS "${_IBMW_XML_WIDE_SRV}")
  message(FATAL_ERROR "Missing minimal-SDK service definition: ${_IBMW_XML_WIDE_SRV}")
endif()
set(_IBMW_XML_WIDE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_IBMW_XML_WIDE_GEN_H "${_IBMW_XML_WIDE_GEN_DIR}/SerializePoseGraph.ibmw_rpc.srv.h")
set(_IBMW_XML_WIDE_GEN_CC "${_IBMW_XML_WIDE_GEN_DIR}/SerializePoseGraph.ibmw_rpc.srv.cc")
set(_IBMW_XML_WIDE_FIX_GENERATED_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/remove_generated_rclcpp_includes.cmake")
add_custom_command(
  OUTPUT "${_IBMW_XML_WIDE_GEN_H}" "${_IBMW_XML_WIDE_GEN_CC}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_IBMW_XML_WIDE_GEN_DIR}"
  COMMAND Python3::Interpreter
    "${CMAKE_SOURCE_DIR}/src/tools/ros2_gen_rpc/ros2_gen_ibmw_cpp_rpc.py"
    --pkg_name=slam_toolbox
    --srv_file=${_IBMW_XML_WIDE_SRV}
    --output_path=${_IBMW_XML_WIDE_GEN_DIR}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${_IBMW_XML_WIDE_GEN_CC}
    -P "${_IBMW_XML_WIDE_FIX_GENERATED_SOURCE}"
  DEPENDS
    "${_IBMW_XML_WIDE_SRV}"
    "${CMAKE_SOURCE_DIR}/src/tools/ros2_gen_rpc/ros2_gen_ibmw_cpp_rpc.py"
    "${_IBMW_XML_WIDE_FIX_GENERATED_SOURCE}"
  VERBATIM)

add_library(ibmw_dds_xml_wide_rpc STATIC "${_IBMW_XML_WIDE_GEN_CC}")
target_include_directories(ibmw_dds_xml_wide_rpc PUBLIC "${_IBMW_XML_WIDE_GEN_DIR}")
target_link_libraries(ibmw_dds_xml_wide_rpc PUBLIC
  ibmw::extensions::ros2_types
  slam_toolbox::slam_toolbox__rosidl_typesupport_fastrtps_cpp)

set(_IBMW_XML_WIDE_TYPESUPPORT_TARGETS
  sensor_msgs::sensor_msgs__rosidl_typesupport_fastrtps_cpp
  nav_msgs::nav_msgs__rosidl_typesupport_fastrtps_cpp
  tf2_msgs::tf2_msgs__rosidl_typesupport_fastrtps_cpp
  visualization_msgs::visualization_msgs__rosidl_typesupport_fastrtps_cpp
  diagnostic_msgs::diagnostic_msgs__rosidl_typesupport_fastrtps_cpp
  slam_toolbox::slam_toolbox__rosidl_typesupport_fastrtps_cpp)
set(_IBMW_XML_WIDE_LIBS
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::base
  ibmw_dds_extension
  ibmw::extensions::ros2_types
  ibmw_dds_xml_wide_rpc
  yaml-cpp::yaml-cpp
  slam_toolbox::slam_toolbox__rosidl_typesupport_cpp
  ${_IBMW_XML_WIDE_TYPESUPPORT_TARGETS})

add_executable(ibmw_dds_xml_wide_publisher_app
  wide_publisher_app.cc wide_publisher_module.cc)
target_include_directories(ibmw_dds_xml_wide_publisher_app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${_IBMW_XML_WIDE_GEN_DIR}")
target_link_libraries(ibmw_dds_xml_wide_publisher_app PRIVATE ${_IBMW_XML_WIDE_LIBS})

add_executable(ibmw_dds_xml_wide_subscriber_app
  wide_subscriber_app.cc wide_subscriber_module.cc)
target_include_directories(ibmw_dds_xml_wide_subscriber_app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${_IBMW_XML_WIDE_GEN_DIR}")
target_link_libraries(ibmw_dds_xml_wide_subscriber_app PRIVATE ${_IBMW_XML_WIDE_LIBS})

foreach(_target IN ITEMS
    ibmw_dds_xml_publisher_app
    ibmw_dds_xml_subscriber_app
    ibmw_dds_xml_wide_publisher_app
    ibmw_dds_xml_wide_subscriber_app)
  set_target_properties(${_target} PROPERTIES
    BUILD_RPATH ""
    INSTALL_RPATH ""
    INSTALL_RPATH_USE_LINK_PATH FALSE
    SKIP_BUILD_RPATH TRUE
    BUILD_WITH_INSTALL_RPATH FALSE)
endforeach()
