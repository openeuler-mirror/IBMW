#!/bin/bash
# Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Ibmw is licensed under Mulan PSL v2.

echo "=========================================="
echo "ROS2-Ibmw Interop Test"
echo "Ibmw Publisher + ROS2 Listener"
echo "=========================================="

# 检查ROS2环境
if [ -z "$ROS_DISTRO" ]; then
  echo "Error: ROS2 not sourced. Please run:"
  echo "  source /opt/ros/${ROS_DISTRO:-humble}/setup.bash"
  exit 1
fi

echo "ROS2 Distribution: $ROS_DISTRO"
echo ""

# 启动Ibmw发布者
echo "Starting Ibmw Publisher..."
./ros2_interop_app ./cfg/ros2_interop_cfg.yaml &
IBMW_PID=$!

sleep 2

# 启动ROS2订阅者
echo "Starting ROS2 Listener..."
ros2 run demo_nodes_cpp listener &
ROS2_PID=$!

echo ""
echo "=========================================="
echo "Both nodes started successfully"
echo "Ibmw PID: $IBMW_PID"
echo "ROS2 PID: $ROS2_PID"
echo ""
echo "You should see:"
echo "  - Ibmw Publisher publishing messages"
echo "  - ROS2 Listener receiving messages"
echo ""
echo "Press Ctrl+C to stop"
echo "=========================================="
echo ""

# 等待用户中断
trap "echo ''; echo 'Stopping...'; kill $IBMW_PID $ROS2_PID 2>/dev/null; exit" SIGINT SIGTERM

wait
