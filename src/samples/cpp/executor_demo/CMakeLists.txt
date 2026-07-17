# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set_namespace()

add_subdirectory(managed/executor_module)
add_subdirectory(managed/executor_co_module)
add_subdirectory(managed/executor_co_loop_module)
add_subdirectory(managed/real_time_module)
add_subdirectory(managed/timer_module)
add_subdirectory(package/executor_pkg)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
elseif(CMAKE_SYSTEM_NAME MATCHES "Windows")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/win)
else()
  message(FATAL_ERROR "Unsupport os")
endif()

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})
add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS ibmw::core::main
          ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::executor_pkg)
