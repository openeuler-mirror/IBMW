# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# DDS IDL RPC code generation tool
# This is a Python script that generates RPC wrappers from DDS IDL service definition YAML files

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})

# Set global property so that DdsIdlRpcGenCode.cmake can find the tool
set_property(GLOBAL PROPERTY DDS_IDL_RPC_GEN_CODE_TOOL_PATH_PROPERTY ${CMAKE_CURRENT_SOURCE_DIR})

# Install the DDS IDL RPC generation script
install(PROGRAMS dds_idl_gen_rpc.py DESTINATION bin)
