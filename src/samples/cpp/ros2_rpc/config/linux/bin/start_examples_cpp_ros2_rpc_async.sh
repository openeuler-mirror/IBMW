#!/bin/bash

source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
export LD_LIBRARY_PATH="/lib/$(uname -m)-linux-gnu:/opt/ros/${ROS_DISTRO:-humble}/lib:$(pwd)/lib:$LD_LIBRARY_PATH"
source share/example_ros2/local_setup.bash

./ibmw run ./cfg/examples_cpp_ros2_rpc_async_cfg.yaml
