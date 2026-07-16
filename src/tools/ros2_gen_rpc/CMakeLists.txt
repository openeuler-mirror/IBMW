# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_property(GLOBAL PROPERTY ROS2_IBMW_RPC_GEN_CODE_TOOL_PATH_PROPERTY "${CMAKE_CURRENT_SOURCE_DIR}")
include(${CMAKE_CURRENT_SOURCE_DIR}/Ros2IbmwRpcGenCode.cmake)
if(IBMW_INSTALL)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/Ros2IbmwRpcGenCode.cmake DESTINATION cmake)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ros2_gen_ibmw_cpp_rpc.py DESTINATION bin)
endif()
