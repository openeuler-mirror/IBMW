# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

# DDS IDL RPC code generation tool
add_subdirectory(dds_idl_gen_rpc)

# ROS2 RPC code generation tool
if(IBMW_BUILD_WITH_ROS2)
  add_subdirectory(ros2_gen_rpc)
endif()

# ibmw CLI tool
if(IBMW_BUILD_CLI_TOOLS)
  add_subdirectory(ibmw_cli)
endif()
