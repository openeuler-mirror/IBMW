cmake_minimum_required(VERSION 3.24)

if(NOT IBMW_BUILD_ROS2_TYPES)
  message(STATUS "Skip ros2_point_demo because IBMW_BUILD_ROS2_TYPES is OFF")
  return()
endif()

project(ros2_point_demo)

if(CMAKE_CROSSCOMPILING AND NOT IBMW_TARGET_ROS2_DEPS_PREFIX)
  message(FATAL_ERROR
    "ros2_point_demo cross builds require IBMW_TARGET_ROS2_DEPS_PREFIX to point "
    "at target ROS 2 type-support dependencies.")
endif()

# Find ROS2
ibmw_find_target_package(ament_cmake REQUIRED FEATURE ros2-point-demo)
ibmw_find_target_package(geometry_msgs REQUIRED FEATURE ros2-point-demo)

# Point Publisher Module
add_library(point_publisher_module STATIC
  managed/point_publisher_module/point_publisher_module.cc
)

target_include_directories(point_publisher_module PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
)

target_link_libraries(point_publisher_module
  ibmw::api::cpp
  ibmw::extensions::ros2_types
  ${geometry_msgs_TARGETS}
)

# Force link the entire impl library to ensure static registrars are initialized
if(APPLE)
  target_link_libraries(point_publisher_module
    -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(point_publisher_module
    -Wl,--whole-archive
    ibmw::extensions::ros2_types_impl
    -Wl,--no-whole-archive
  )
else()
  target_link_libraries(point_publisher_module
    ibmw::extensions::ros2_types_impl
  )
endif()

# Point Subscriber Module
add_library(point_subscriber_module STATIC
  managed/point_subscriber_module/point_subscriber_module.cc
)

target_include_directories(point_subscriber_module PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
)

target_link_libraries(point_subscriber_module
  ibmw::api::cpp
  ibmw::extensions::ros2_types
  ${geometry_msgs_TARGETS}
)

# Force link the entire impl library to ensure static registrars are initialized
if(APPLE)
  target_link_libraries(point_subscriber_module
    -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(point_subscriber_module
    -Wl,--whole-archive
    ibmw::extensions::ros2_types_impl
    -Wl,--no-whole-archive
  )
else()
  target_link_libraries(point_subscriber_module
    ibmw::extensions::ros2_types_impl
  )
endif()

# ROS2 Point App
add_executable(ros2_point_app
  standalone/ros2_point_app/main.cc
)

target_link_libraries(ros2_point_app
  point_publisher_module
  point_subscriber_module
  ibmw::core::engine
)

# Install
install(TARGETS ros2_point_app
  RUNTIME DESTINATION bin
)

install(DIRECTORY config/
  DESTINATION share/${PROJECT_NAME}
  PATTERN ".*" EXCLUDE
)
