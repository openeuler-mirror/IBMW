#!/bin/bash

source /opt/ros/humble/setup.bash
export LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$LD_LIBRARY_PATH"
source share/example_ros2/local_setup.bash

./ibmw run ./cfg/examples_cpp_ros2_chn_single_pkg_cfg.yaml
