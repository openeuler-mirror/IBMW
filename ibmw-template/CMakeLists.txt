# IBMW External Project Template
#
# This is a starter project for building IBMW modules and applications
# outside the IBMW source tree.
#
# Prerequisites:
#   - IBMW must be installed (cmake --install <build-dir> --prefix <install-prefix>)
#   - Set CMAKE_PREFIX_PATH to point to the IBMW install prefix
#   - ROS2 (Humble) sourced if building ROS2 demos
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH="$IBMW_INSTALL_PREFIX"
#   make
#
# Demos provided:
#   1. Minimal module        (pkg + app mode)  — MyModule (Setup/Run/Stop lifecycle)
#   2. DDS Pub/Sub transport (pkg + app mode)  — PublisherModule + SubscriberModule
#   3. DDS RPC service       (pkg + app mode)  — RpcServerModule + RpcClientModule (coroutine)
#   4. ROS2 Pub/Sub          (pkg + app mode)  — Ros2PublisherModule + Ros2SubscriberModule
#   5. ROS2 RPC              (pkg + app mode)  — Ros2RpcServerModule + Ros2RpcClientModule
#   6. JSON Pub/Sub          (pkg + app mode)  — JsonPublisherModule + JsonSubscriberModule
#   7. JSON RPC              (pkg + app mode)  — JsonRpcServerModule + JsonRpcClientModule
#   8. Bring-your-own IDL    (pkg + app mode)  — custom MyMsg.idl + manual cdr_impl.cc

cmake_minimum_required(VERSION 3.24)
project(my_ibmw_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Find IBMW  (also loads IbmwHelpers.cmake: ibmw_add_module/pkg/app/etc.)
# --------------------------------------------------------------------------
find_package(ibmw 0.1.0 REQUIRED)

# --------------------------------------------------------------------------
# nlohmann_json — required for JSON demos (6 & 7).
# IBMW uses it internally but does not install it, so external projects
# must fetch it themselves.
# --------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/archive/v3.11.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ==========================================================================
# 1. Minimal module demo (no transport)
# ==========================================================================

ibmw_add_module(my_module
  SRCS src/my_module/my_module.h src/my_module/my_module.cc)

ibmw_add_pkg(my_module_pkg
  SRCS src/my_module_pkg/pkg_main.cc
  MODULES my_module)

# App mode embeds the engine directly (no modules needed since my_module
# is registered via header include in main.cc)
add_executable(my_app src/my_app/main.cc)
target_link_libraries(my_app PRIVATE ibmw::core::engine)

# ==========================================================================
# 2. DDS Pub/Sub transport demo (using IBMW-shipped ExampleEventMsg DDS IDL type)
# ==========================================================================
#
# `ibmw::schema::example_idl_gencode` already declares
# `WRAP_IDLS ExampleEvent ...` in its own CMakeLists, which means the auto-
# generated CdrImpl wrapper TUs (one per IDL stem) are baked into the schema
# package's STATIC archive and shipped through find_package(ibmw).
# Therefore no hand-written `example_event_cdr_impl.cc` is needed: we use
# ibmw_add_dds_messages() in its "schema-shipped" (INTERFACE) mode by simply
# omitting the CDR_IMPLS argument. See docs/architecture/overview.md for the
# design rationale.
# For the "bring-your-own-IDL" mode (mode 2) see Demo 8 below.

ibmw_add_dds_messages(example_event_cdr
  IDL_GENCODE_TARGET ibmw::schema::example_idl_gencode)

ibmw_add_module(publisher_module
  SRCS src/publisher_module/publisher_module.cc
  DEPS ibmw::extensions::dds_types ibmw::schema::example_idl_gencode example_event_cdr)

ibmw_add_module(subscriber_module
  SRCS src/subscriber_module/subscriber_module.cc
  DEPS ibmw::extensions::dds_types ibmw::schema::example_idl_gencode example_event_cdr)

ibmw_add_pkg(pubsub_pkg
  SRCS src/pubsub_pkg/pkg_main.cc
  MODULES publisher_module subscriber_module)

ibmw_add_app(pubsub_app
  SRCS src/pubsub_app/main.cc
  MODULES publisher_module subscriber_module)

# ==========================================================================
# 3. DDS RPC service demo (using IBMW-shipped ExampleService RPC generated code)
# ==========================================================================
#
# Same "schema-shipped" mode as Demo 2 — `ibmw::schema::example_idl_gencode`
# carries the WRAP_IDLS wrapper TU for ExampleRpc + ExampleCommon, and the
# RPC gencode target only adds the service-specific glue (no extra CDR
# symbols), so CDR_IMPLS is omitted here as well.

ibmw_add_dds_rpc(example_rpc_cdr
  IDL_GENCODE_TARGET ibmw::schema::example_idl_gencode
  RPC_GENCODE_TARGET ibmw::schema::example_rpc_gencode)

ibmw_add_module(rpc_server_module
  SRCS src/rpc_server_module/rpc_server_module.cc
  DEPS ibmw::extensions::dds_types ibmw::schema::example_rpc_gencode example_rpc_cdr)

ibmw_add_module(rpc_client_module
  SRCS src/rpc_client_module/rpc_client_module.cc
  DEPS ibmw::extensions::dds_types ibmw::schema::example_rpc_gencode example_rpc_cdr)

ibmw_add_pkg(rpc_pkg
  SRCS src/rpc_pkg/pkg_main.cc
  MODULES rpc_server_module rpc_client_module)

ibmw_add_app(rpc_app
  SRCS src/rpc_app/main.cc
  MODULES rpc_server_module rpc_client_module)

# ==========================================================================
# 4. ROS2 Pub/Sub demo (using example_ros2::msg::RosTestMsg)
# 5. ROS2 RPC demo  (using example_ros2::srv::RosTestRpc)
#
# These demos depend on the optional ROS2 example schema package
# `example_ros2`, which IBMW only installs when built with
# -DIBMW_BUILD_WITH_ROS2=ON together with the ament/rosidl toolchain. If the
# package is not available, the demos are gracefully skipped so that the
# template can still be built on minimal IBMW installs and on systems
# without ROS2 -- in line with the deprecation note in the README. To
# explicitly require them, configure with -DIBMW_TEMPLATE_REQUIRE_ROS2=ON.
# ==========================================================================

option(IBMW_TEMPLATE_REQUIRE_ROS2
  "Fail configuration when example_ros2 / ROS2 RPC schema are missing" OFF)

if(IBMW_TEMPLATE_REQUIRE_ROS2)
  find_package(example_ros2 REQUIRED)
else()
  find_package(example_ros2 QUIET)
endif()

if(example_ros2_FOUND
   AND TARGET ibmw::schema::example_ros2_ibmw_rpc_gencode)

  ibmw_add_module(ros2_publisher_module
    SRCS src/ros2_publisher_module/ros2_publisher_module.cc
    DEPS ibmw::extensions::ros2_types example_ros2::example_ros2__rosidl_generator_cpp)

  ibmw_add_module(ros2_subscriber_module
    SRCS src/ros2_subscriber_module/ros2_subscriber_module.cc
    DEPS ibmw::extensions::ros2_types example_ros2::example_ros2__rosidl_generator_cpp)

  ibmw_add_pkg(ros2_pubsub_pkg
    SRCS src/ros2_pubsub_pkg/pkg_main.cc
    MODULES ros2_publisher_module ros2_subscriber_module)

  ibmw_add_app(ros2_pubsub_app
    SRCS src/ros2_pubsub_app/main.cc
    MODULES ros2_publisher_module ros2_subscriber_module)

  ibmw_add_module(ros2_rpc_server_module
    SRCS src/ros2_rpc_server_module/ros2_rpc_server_module.cc
    DEPS ibmw::extensions::ros2_types ibmw::schema::example_ros2_ibmw_rpc_gencode)

  ibmw_add_module(ros2_rpc_client_module
    SRCS src/ros2_rpc_client_module/ros2_rpc_client_module.cc
    DEPS ibmw::extensions::ros2_types ibmw::schema::example_ros2_ibmw_rpc_gencode)

  ibmw_add_pkg(ros2_rpc_pkg
    SRCS src/ros2_rpc_pkg/pkg_main.cc
    MODULES ros2_rpc_server_module ros2_rpc_client_module)

  ibmw_add_app(ros2_rpc_app
    SRCS src/ros2_rpc_app/main.cc
    MODULES ros2_rpc_server_module ros2_rpc_client_module)

  message(STATUS "[ibmw-template] Demo 4/5 (ROS2 pub/sub, ROS2 RPC) ENABLED.")
else()
  message(STATUS "[ibmw-template] Demo 4/5 (ROS2 pub/sub, ROS2 RPC) SKIPPED: "
    "example_ros2 package or ibmw::schema::example_ros2_ibmw_rpc_gencode "
    "target not found. Rebuild IBMW with -DIBMW_BUILD_WITH_ROS2=ON to enable, "
    "or pass -DIBMW_TEMPLATE_REQUIRE_ROS2=ON to make this an error.")
endif()

# ==========================================================================
# 6. JSON Pub/Sub demo (custom struct + nlohmann_json, no IDL)
# ==========================================================================

ibmw_add_module(json_publisher_module
  SRCS src/json_publisher_module/json_publisher_module.cc
  DEPS nlohmann_json::nlohmann_json)

ibmw_add_module(json_subscriber_module
  SRCS src/json_subscriber_module/json_subscriber_module.cc
  DEPS nlohmann_json::nlohmann_json)

ibmw_add_pkg(json_pubsub_pkg
  SRCS src/json_pubsub_pkg/pkg_main.cc
  MODULES json_publisher_module json_subscriber_module)

ibmw_add_app(json_pubsub_app
  SRCS src/json_pubsub_app/main.cc
  MODULES json_publisher_module json_subscriber_module)

# ==========================================================================
# 7. JSON RPC demo (custom struct + nlohmann_json, no IDL)
# ==========================================================================

ibmw_add_module(json_rpc_server_module
  SRCS src/json_rpc_server_module/json_rpc_server_module.cc
  DEPS nlohmann_json::nlohmann_json)

ibmw_add_module(json_rpc_client_module
  SRCS src/json_rpc_client_module/json_rpc_client_module.cc
  DEPS nlohmann_json::nlohmann_json)

ibmw_add_pkg(json_rpc_pkg
  SRCS src/json_rpc_pkg/pkg_main.cc
  MODULES json_rpc_server_module json_rpc_client_module)

ibmw_add_app(json_rpc_app
  SRCS src/json_rpc_app/main.cc
  MODULES json_rpc_server_module json_rpc_client_module)

# ==========================================================================
# 8. Bring-your-own IDL DDS Pub/Sub demo (custom MyMsg.idl, NOT shipped by ibmw)
# ==========================================================================
#
# Demos 2 and 3 reuse IBMW-shipped schemas whose CMakeLists declared
# `WRAP_IDLS ...`, so their CDR symbols are baked into the schema package's
# STATIC archive — no per-project cdr_impl.cc is needed.
#
# This demo walks through the OPPOSITE workflow: you bring your own .idl
# file. Steps:
#
#   1) Run `add_dds_idl_gencode_target()` to invoke fastddsgen and produce
#      `<MyMsg>.hpp`, `<MyMsg>CdrAux.ipp`, etc. in the build tree.
#      You can OPT-IN to WRAP_IDLS to let the helper auto-emit a wrapper TU
#      so you skip writing cdr_impl.cc yourself; here we deliberately do NOT
#      pass WRAP_IDLS to demonstrate the manual ODR-safe path that you must
#      use whenever the py post-processor cannot yet handle your IDL pattern
#      (unions, sequence<NestedStruct>, etc.).
#
#   2) Hand-write a single-line `my_msg_cdr_impl.cc` that `#include`s the
#      generated `MyMsgCdrAux.ipp`. This TU materializes the inline FastCDR
#      specializations exactly once per process.
#
#   3) Pass the .cc to `ibmw_add_dds_messages(... CDR_IMPLS ...)`. The
#      helper switches into "STATIC" mode (mode 2) automatically; modules
#      then link the resulting target as a normal CMake dependency.

include(DdsIdlGenCode)

add_dds_idl_gencode_target(
  TARGET_NAME my_msg_idl_gencode
  IDL_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg/MyMsg.idl
  GENCODE_PATH ${CMAKE_CURRENT_BINARY_DIR}/gencode/my_msg
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/my_msg)

ibmw_add_dds_messages(my_msg_cdr
  IDL_GENCODE_TARGET my_msg_idl_gencode
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src
  CDR_IMPLS src/my_msg/my_msg_cdr_impl.cc)

ibmw_add_module(my_msg_publisher_module
  SRCS src/my_msg_publisher_module/my_msg_publisher_module.cc
  DEPS ibmw::extensions::dds_types my_msg_idl_gencode my_msg_cdr)

ibmw_add_module(my_msg_subscriber_module
  SRCS src/my_msg_subscriber_module/my_msg_subscriber_module.cc
  DEPS ibmw::extensions::dds_types my_msg_idl_gencode my_msg_cdr)

ibmw_add_pkg(my_msg_pubsub_pkg
  SRCS src/my_msg_pubsub_pkg/pkg_main.cc
  MODULES my_msg_publisher_module my_msg_subscriber_module)

ibmw_add_app(my_msg_pubsub_app
  SRCS src/my_msg_pubsub_app/main.cc
  MODULES my_msg_publisher_module my_msg_subscriber_module)

# --------------------------------------------------------------------------
# Copy config files to build directory
# --------------------------------------------------------------------------
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cfg DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

message(STATUS "")
message(STATUS "IBMW External Project configured successfully!")
message(STATUS "")
message(STATUS "  1. Minimal module:")
message(STATUS "       Pkg:  ibmw run cfg/my_module_pkg.yaml")
message(STATUS "       App:  ./my_app cfg/my_app.yaml")
message(STATUS "")
message(STATUS "  2. DDS Pub/Sub transport:")
message(STATUS "       Pkg:  ibmw run cfg/pubsub_pkg.yaml")
message(STATUS "       App:  ./pubsub_app cfg/pubsub_app.yaml")
message(STATUS "")
message(STATUS "  3. DDS RPC service:")
message(STATUS "       Pkg:  ibmw run cfg/rpc_pkg.yaml")
message(STATUS "       App:  ./rpc_app cfg/rpc_app.yaml")
message(STATUS "")
message(STATUS "  4. ROS2 Pub/Sub:")
message(STATUS "       Pkg:  ibmw run cfg/ros2_pubsub_pkg.yaml")
message(STATUS "       App:  ./ros2_pubsub_app cfg/ros2_pubsub_app.yaml")
message(STATUS "")
message(STATUS "  5. ROS2 RPC:")
message(STATUS "       Pkg:  ibmw run cfg/ros2_rpc_pkg.yaml")
message(STATUS "       App:  ./ros2_rpc_app cfg/ros2_rpc_app.yaml")
message(STATUS "")
message(STATUS "  6. JSON Pub/Sub (no IDL):")
message(STATUS "       Pkg:  ibmw run cfg/json_pubsub_pkg.yaml")
message(STATUS "       App:  ./json_pubsub_app cfg/json_pubsub_app.yaml")
message(STATUS "")
message(STATUS "  7. JSON RPC (no IDL):")
message(STATUS "       Pkg:  ibmw run cfg/json_rpc_pkg.yaml")
message(STATUS "       App:  ./json_rpc_app cfg/json_rpc_app.yaml")
message(STATUS "")
message(STATUS "  8. Bring-your-own IDL Pub/Sub (custom MyMsg.idl):")
message(STATUS "       Pkg:  ibmw run cfg/my_msg_pubsub_pkg.yaml")
message(STATUS "       App:  ./my_msg_pubsub_app cfg/my_msg_pubsub_app.yaml")
message(STATUS "")
