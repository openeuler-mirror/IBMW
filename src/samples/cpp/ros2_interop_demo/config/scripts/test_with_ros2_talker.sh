#!/bin/bash
# Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Ibmw is licensed under Mulan PSL v2.

echo "=========================================="
echo "ROS2-Ibmw Interop Test"
echo "Ibmw Subscriber + ROS2 Talker"
echo "=========================================="

# 检查ROS2环境
if [ -z "$ROS_DISTRO" ]; then
  echo "Error: ROS2 not sourced. Please run:"
  echo "  source /opt/ros/humble/setup.bash"
  exit 1
fi

echo "ROS2 Distribution: $ROS_DISTRO"
echo ""

# 启动Ibmw订阅者
echo "Starting Ibmw Subscriber..."
./ros2_interop_app ./cfg/ros2_interop_cfg.yaml &
IBMW_PID=$!

sleep 2

# 启动ROS2发布者
echo "Starting ROS2 Talker..."
ros2 run demo_nodes_cpp talker &
ROS2_PID=$!

echo ""
echo "=========================================="
echo "Both nodes started successfully"
echo "Ibmw PID: $IBMW_PID"
echo "ROS2 PID: $ROS2_PID"
echo ""
echo "You should see:"
echo "  - ROS2 Talker publishing messages"
echo "  - Ibmw Subscriber receiving messages"
echo ""
echo "Press Ctrl+C to stop"
echo "=========================================="
echo ""

# 等待用户中断
trap "echo ''; echo 'Stopping...'; kill $IBMW_PID $ROS2_PID 2>/dev/null; exit" SIGINT SIGTERM

wait
