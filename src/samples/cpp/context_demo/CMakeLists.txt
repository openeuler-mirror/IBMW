# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

# NOTE: The legacy hand-written `example_event_cdr_impl.cc` /
# `example_rpc_cdr_impl.cc` (and their STATIC wrappers
# `context_event_cdr_impl` / `context_rpc_cdr_impl`) have been removed.
# CDR (de)serialization for ExampleEventMsg, ExampleRpc* and ExampleCommon
# is now provided by `ibmw::schema::example_idl_gencode` whose
# CMakeLists.txt declares `WRAP_IDLS ExampleCommon ExampleEvent ExampleRpc`.
# See docs/contributing/architecture-rules.md.

# Modules
add_subdirectory(managed/chn_publisher_module)
add_subdirectory(managed/chn_subscriber_on_executor_module)
add_subdirectory(managed/chn_subscriber_inline_module)
add_subdirectory(managed/rpc_client_module)
add_subdirectory(managed/rpc_server_inline_module)
add_subdirectory(managed/rpc_server_on_executor_module)
add_subdirectory(managed/executor)
add_subdirectory(managed/logger)

# Package
add_subdirectory(package)

# Install resources
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "Unsupported os")
endif()

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS ibmw::core::main
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::package)
