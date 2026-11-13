#!/bin/bash
# Environment Check Script for ROS2-Ibmw Interop Testing
# This script checks for any lingering processes that might interfere with testing

set -e

echo "=========================================="
echo "ROS2-Ibmw Environment Check"
echo "=========================================="

# Source ROS2 environment
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash

echo ""
echo "1. Checking for running Ibmw processes..."
IBMW_PROCS=$(ps aux | grep -E "(ros2_interop_app|ros2_pub_only_app|ros2_sub_only_app)" | grep -v grep || true)
if [ -n "$IBMW_PROCS" ]; then
    echo "⚠️  WARNING: Found running Ibmw processes:"
    echo "$IBMW_PROCS"
    echo ""
    read -p "Kill these processes? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        pkill -f "ros2_interop_app" || true
        pkill -f "ros2_pub_only_app" || true
        pkill -f "ros2_sub_only_app" || true
        echo "✅ Processes killed"
    fi
else
    echo "✅ No Ibmw processes found"
fi

echo ""
echo "2. Checking for ROS2 demo_nodes_cpp processes..."
ROS2_PROCS=$(ps aux | grep -E "demo_nodes_cpp" | grep -v grep || true)
if [ -n "$ROS2_PROCS" ]; then
    echo "⚠️  WARNING: Found running ROS2 demo nodes:"
    echo "$ROS2_PROCS"
    echo ""
    read -p "Kill these processes? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        pkill -f "demo_nodes_cpp" || true
        echo "✅ Processes killed"
    fi
else
    echo "✅ No ROS2 demo nodes found"
fi

echo ""
echo "3. Checking ROS2 topics..."
TOPICS=$(ros2 topic list 2>/dev/null || true)
if echo "$TOPICS" | grep -q "/chatter"; then
    echo "⚠️  WARNING: /chatter topic already exists"
    echo "Current topics:"
    echo "$TOPICS"
    echo ""
    echo "This might be from a previous test. Consider waiting a few seconds for cleanup."
else
    echo "✅ No /chatter topic found (clean state)"
fi

echo ""
echo "4. Checking for demo_nodes_cpp package..."
if ros2 pkg list | grep -q "demo_nodes_cpp"; then
    echo "✅ demo_nodes_cpp package found"
    TALKER_PATH=$(ros2 pkg prefix demo_nodes_cpp)/lib/demo_nodes_cpp/talker
    LISTENER_PATH=$(ros2 pkg prefix demo_nodes_cpp)/lib/demo_nodes_cpp/listener
    if [ -f "$TALKER_PATH" ]; then
        echo "   - talker: $TALKER_PATH"
    else
        echo "   ⚠️  talker not found at expected location"
    fi
    if [ -f "$LISTENER_PATH" ]; then
        echo "   - listener: $LISTENER_PATH"
    else
        echo "   ⚠️  listener not found at expected location"
    fi
else
    echo "❌ ERROR: demo_nodes_cpp package not found!"
    echo "   Please install: sudo apt install ros-${ROS_DISTRO:-humble}-demo-nodes-cpp"
    exit 1
fi

echo ""
echo "5. Checking DDS domain..."
if [ -n "$ROS_DOMAIN_ID" ]; then
    echo "⚠️  ROS_DOMAIN_ID is set to: $ROS_DOMAIN_ID"
    echo "   Make sure this matches your Ibmw config (default: 0)"
else
    echo "✅ ROS_DOMAIN_ID not set (using default: 0)"
fi

echo ""
echo "6. Checking required files..."
if [ ! -f "./libibmw_dds_plugin.so" ]; then
    echo "❌ ERROR: libibmw_dds_plugin.so not found in current directory"
    exit 1
else
    echo "✅ libibmw_dds_plugin.so found"
fi

if [ ! -f "./ros2_pub_only_app" ]; then
    echo "❌ ERROR: ros2_pub_only_app not found"
    exit 1
else
    echo "✅ ros2_pub_only_app found"
fi

if [ ! -f "./ros2_sub_only_app" ]; then
    echo "❌ ERROR: ros2_sub_only_app not found"
    exit 1
else
    echo "✅ ros2_sub_only_app found"
fi

echo ""
echo "=========================================="
echo "Environment Check Complete!"
echo "=========================================="
echo ""
echo "Ready to run interop tests."
echo ""
