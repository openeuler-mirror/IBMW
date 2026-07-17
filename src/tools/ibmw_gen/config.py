# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ibmw-gen runtime configuration.
#
# Purpose
#   Centralize paths that ibmw-gen emits into generated CMakeLists.txt so
#   that integrators can override them without forking the generator.
#   Today only the ROS2 RPC gencode tool path is exposed. The configuration
#   surface can grow alongside the ros2_gen_rpc tooling.
#
# Override mechanism
#   Each setting may be overridden via an environment variable read at
#   generator invocation time. Defaults preserve historical behavior so
#   existing samples and user projects regenerate identically when no
#   env vars are set. See docs/contributing/testing.md for validation guidance.
#
# Naming convention
#   IBMW_GEN_<UPPER_SNAKE_NAME>
#
# This module has no third-party dependencies and is import-safe under
# both `python3 ibmw_gen.py` and the generator unit-test runner.

import os

# --- ROS2 RPC gencode integration --------------------------------------------
#
# Historically, `cmake_gen.py` hard-coded both integration paths:
#
#   include(${ibmw_DIR}/../../../src/tools/ros2_gen_rpc/Ros2IbmwRpcGenCode.cmake)
#   set_property(... ${ibmw_DIR}/../../../src/tools/ros2_gen_rpc)
#
# These two paths must agree (one is the .cmake file, the other its parent
# directory). They are computed relative to ibmw's installed CMake config
# directory via `${ibmw_DIR}`.

# Default: preserve the established in-tree path layout.
_DEFAULT_ROS2_RPC_GEN_DIR = "${ibmw_DIR}/../../../src/tools/ros2_gen_rpc"
_DEFAULT_ROS2_RPC_GEN_CMAKE = (
    "${ibmw_DIR}/../../../src/tools/ros2_gen_rpc/Ros2IbmwRpcGenCode.cmake"
)


def ros2_rpc_gen_dir() -> str:
    """Directory containing Ros2IbmwRpcGenCode.cmake and the gen tool.

    Override: IBMW_GEN_ROS2_CMAKE_DIR
    """
    return os.environ.get("IBMW_GEN_ROS2_CMAKE_DIR", _DEFAULT_ROS2_RPC_GEN_DIR)


def ros2_rpc_gen_cmake_path() -> str:
    """Full path to Ros2IbmwRpcGenCode.cmake (the file include()'d by gen).

    Override: IBMW_GEN_ROS2_CMAKE_PATH
    Falls back to ``ros2_rpc_gen_dir()/Ros2IbmwRpcGenCode.cmake`` when the
    dir-only override is set, so callers only need to set one of the two.
    """
    explicit = os.environ.get("IBMW_GEN_ROS2_CMAKE_PATH")
    if explicit:
        return explicit
    dir_override = os.environ.get("IBMW_GEN_ROS2_CMAKE_DIR")
    if dir_override:
        return f"{dir_override}/Ros2IbmwRpcGenCode.cmake"
    return _DEFAULT_ROS2_RPC_GEN_CMAKE
