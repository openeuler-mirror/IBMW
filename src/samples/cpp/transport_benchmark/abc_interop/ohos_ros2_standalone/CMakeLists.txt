cmake_minimum_required(VERSION 3.20)

# This project is intentionally separate from IBMW's build graph and ROS2 tier
# switches. Infer the checkout from this file, while allowing relocated trees.
get_filename_component(_inferred_ibmw_source_dir
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(IBMW_SOURCE_DIR "${_inferred_ibmw_source_dir}" CACHE PATH
  "IBMW repository source root")
set(IBMW_OHOS_ROS2_PREFIX "" CACHE PATH
  "External full ROS2 installation prefix built for OHOS")

if(NOT EXISTS "${IBMW_SOURCE_DIR}/src/samples/cpp/transport_benchmark/abc_interop/ros2_endpoint.cc")
  message(FATAL_ERROR
    "IBMW_SOURCE_DIR does not identify an IBMW checkout: ${IBMW_SOURCE_DIR}")
endif()

# Select the repository toolchain by default; callers may override it before
# project() when a compatible relocated repository toolchain is required.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
  set(CMAKE_TOOLCHAIN_FILE
    "${IBMW_SOURCE_DIR}/cmake/toolchains/ohos-arm64-v8a.cmake"
    CACHE FILEPATH "OHOS cross-compilation toolchain" FORCE)
endif()
if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(FATAL_ERROR "CMAKE_TOOLCHAIN_FILE does not exist: ${CMAKE_TOOLCHAIN_FILE}")
endif()
if(IBMW_OHOS_ROS2_PREFIX STREQUAL "" OR
   NOT EXISTS "${IBMW_OHOS_ROS2_PREFIX}/share/rclcpp/cmake/rclcppConfig.cmake")
  message(FATAL_ERROR
    "IBMW_OHOS_ROS2_PREFIX must name a full OHOS ROS2 installation")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${IBMW_OHOS_ROS2_PREFIX}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${IBMW_OHOS_ROS2_PREFIX}")

project(ibmw_ohos_abc_ros2_standalone LANGUAGES CXX)

message(STATUS "IBMW source root: ${IBMW_SOURCE_DIR}")
message(STATUS "OHOS toolchain: ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "External OHOS ROS2 prefix: ${IBMW_OHOS_ROS2_PREFIX}")
message(STATUS "Standalone build leaves IBMW_BUILD_WITH_ROS2 and target tiers unchanged")

find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)
find_package(rmw_fastrtps_cpp REQUIRED)

# Relocated OHOS ROS2 exports can retain build-host paths. CMake validates
# these transitive properties even though this C++ endpoint does not use the
# Python generator, so remove only missing absolute includes and Python links.
get_property(_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
foreach(_target IN LISTS _imported_targets)
  get_target_property(_includes "${_target}" INTERFACE_INCLUDE_DIRECTORIES)
  if(_includes AND NOT _includes STREQUAL "_includes-NOTFOUND")
    set(_valid_includes)
    foreach(_include IN LISTS _includes)
      if(NOT IS_ABSOLUTE "${_include}" OR EXISTS "${_include}")
        list(APPEND _valid_includes "${_include}")
      endif()
    endforeach()
    set_target_properties("${_target}" PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_valid_includes}")
  endif()

  get_target_property(_links "${_target}" INTERFACE_LINK_LIBRARIES)
  if(_links AND NOT _links STREQUAL "_links-NOTFOUND")
    set(_valid_links)
    foreach(_link IN LISTS _links)
      if(NOT _link MATCHES "__rosidl_generator_py$")
        list(APPEND _valid_links "${_link}")
      endif()
    endforeach()
    set_target_properties("${_target}" PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_valid_links}")
  endif()
endforeach()

set(ABC_DIR
  "${IBMW_SOURCE_DIR}/src/samples/cpp/transport_benchmark/abc_interop")
add_executable(ibmw_abc_ros2_endpoint
  "${ABC_DIR}/ros2_endpoint.cc"
  "${ABC_DIR}/ros2_latency_artifact.cc")
target_compile_features(ibmw_abc_ros2_endpoint PRIVATE cxx_std_20)
target_include_directories(ibmw_abc_ros2_endpoint PRIVATE
  "${IBMW_SOURCE_DIR}/src"
  "${ABC_DIR}")
target_link_libraries(ibmw_abc_ros2_endpoint PRIVATE
  rclcpp::rclcpp
  std_msgs::std_msgs__rosidl_generator_cpp
  std_msgs::std_msgs__rosidl_typesupport_cpp)
if(TARGET std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
  target_link_libraries(ibmw_abc_ros2_endpoint PRIVATE
    std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp)
endif()
