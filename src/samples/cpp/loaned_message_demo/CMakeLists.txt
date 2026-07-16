# Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Ibmw is licensed under Mulan PSL v2.

cmake_minimum_required(VERSION 3.24)

if(NOT IBMW_BUILD_ROS2_TYPES)
  message(STATUS "Skip loaned_message_demo because IBMW_BUILD_ROS2_TYPES is OFF")
  return()
endif()

project(loaned_message_demo)

# Find ROS2
find_package(ament_cmake REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)

# Find FastDDS (try fastdds 3.x first, fallback to fastrtps for ROS2 Humble)
if(NOT TARGET eProsima_atomic)
  find_package(fastdds 3 QUIET)
endif()
if(NOT fastdds_FOUND)
  find_package(fastrtps REQUIRED)
  find_package(fastcdr REQUIRED)
  set(fastdds_FOUND TRUE)
  if(NOT TARGET fastdds)
    add_library(fastdds ALIAS fastrtps)
  endif()
  message(STATUS "loaned_message_demo: Using fastrtps ${fastrtps_VERSION}")
else()
  message(STATUS "loaned_message_demo: Using fastdds ${fastdds_VERSION}")
endif()

message(STATUS "Building loaned_message_demo")

# ========== Plain Publisher Module (uses LoanedMessage API) ==========
add_library(plain_publisher_module STATIC
  managed/plain_publisher_module/plain_publisher_module.cc
)

target_include_directories(plain_publisher_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/managed
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(plain_publisher_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
    ${geometry_msgs_TARGETS}
  PRIVATE
    fastdds
)

if(APPLE)
  target_link_libraries(plain_publisher_module
    PUBLIC -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(plain_publisher_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(plain_publisher_module
    PUBLIC ibmw::extensions::ros2_types_impl
  )
endif()

# ========== Non-Plain Publisher Module (uses LoanedMessage API for fallback verification) ==========
add_library(nonplain_publisher_module STATIC
  managed/nonplain_publisher_module/nonplain_publisher_module.cc
)

target_include_directories(nonplain_publisher_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/managed
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(nonplain_publisher_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
    ${std_msgs_TARGETS}
  PRIVATE
    fastdds
)

if(APPLE)
  target_link_libraries(nonplain_publisher_module
    PUBLIC -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(nonplain_publisher_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(nonplain_publisher_module
    PUBLIC ibmw::extensions::ros2_types_impl
  )
endif()

# ========== Config Test Module ==========
add_library(config_test_module STATIC
  managed/config_test_module/config_test_module.cc
)

target_include_directories(config_test_module
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/managed
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(config_test_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
    ${geometry_msgs_TARGETS}
    ${std_msgs_TARGETS}
  PRIVATE
    fastdds
)

if(APPLE)
  target_link_libraries(config_test_module
    PUBLIC -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(config_test_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(config_test_module
    PUBLIC ibmw::extensions::ros2_types_impl
  )
endif()

# ========== Subscriber Module ==========
add_library(subscriber_module STATIC
  managed/subscriber_module/subscriber_module.cc
)

target_include_directories(subscriber_module
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/managed
)

target_link_libraries(subscriber_module
  PUBLIC
    ibmw::api::cpp
    ibmw::extensions::ros2_types
    ${geometry_msgs_TARGETS}
    ${std_msgs_TARGETS}
)

if(APPLE)
  target_link_libraries(subscriber_module
    PUBLIC -Wl,-force_load ibmw::extensions::ros2_types_impl
  )
elseif(UNIX)
  target_link_libraries(subscriber_module
    PUBLIC
      -Wl,--whole-archive
      ibmw::extensions::ros2_types_impl
      -Wl,--no-whole-archive
  )
else()
  target_link_libraries(subscriber_module
    PUBLIC ibmw::extensions::ros2_types_impl
  )
endif()

# The four executables below all link the DDS extension target
# (libibmw_dds_extension) directly to invoke its low-level zero-copy APIs
# (DdsTransportDriver, Ros2StylePublisher, BorrowLoanedMessage). When
# IBMW_BUILD_DDS_EXTENSION=OFF these targets cannot be built; gate them
# on IBMW_HAS_DDS rather than skipping the whole demo so that the
# subscriber-only path (sub_only_app, which does not need the DDS
# extension symbol) keeps building under dds_off.
if(IBMW_HAS_DDS)

# ========== Full Test App ==========
add_executable(loaned_message_app
  standalone/loaned_message_app/main.cc
)

target_include_directories(loaned_message_app
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(loaned_message_app
  PRIVATE
    ibmw::core::engine
    ibmw_dds_extension
    plain_publisher_module
    nonplain_publisher_module
    subscriber_module
    fastdds
)

# ========== Plain Only App ==========
add_executable(plain_pub_only_app
  standalone/plain_pub_only_app/main.cc
)

target_include_directories(plain_pub_only_app
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(plain_pub_only_app
  PRIVATE
    ibmw::core::engine
    ibmw_dds_extension
    plain_publisher_module
    fastdds
)

# ========== Non-Plain Only App ==========
add_executable(nonplain_pub_only_app
  standalone/nonplain_pub_only_app/main.cc
)

target_include_directories(nonplain_pub_only_app
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(nonplain_pub_only_app
  PRIVATE
    ibmw::core::engine
    ibmw_dds_extension
    nonplain_publisher_module
    fastdds
)

# ========== Config Test App ==========
add_executable(config_test_app
  standalone/config_test_app/main.cc
)

target_include_directories(config_test_app
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/extensions
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(config_test_app
  PRIVATE
    ibmw::core::engine
    ibmw_dds_extension
    config_test_module
    fastdds
)

endif()  # IBMW_HAS_DDS

add_custom_target(ibmw_samples_cpp_loaned_message_demo_strict_scripts ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/config/scripts/test_multi_process.sh
    ${CMAKE_BINARY_DIR}/validate_loaned_message_multi_process.sh
  COMMENT "Copy loaned_message_demo strict validation script"
)

# ========== Subscriber Only App ==========
add_executable(sub_only_app
  standalone/sub_only_app/main.cc
)

target_include_directories(sub_only_app
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api

)

target_link_libraries(sub_only_app
  PRIVATE
    subscriber_module
    ibmw::core::engine
)

message(STATUS "loaned_message_demo configured successfully")
