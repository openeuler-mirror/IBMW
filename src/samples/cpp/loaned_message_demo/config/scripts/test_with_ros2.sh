#!/bin/bash
# test_with_ros2.sh - ROS2 互通测试
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_DIR="$SCRIPT_DIR/../cfg"
BUILD_DIR="${BUILD_DIR:-$(cd "$SCRIPT_DIR/../../../../build" 2>/dev/null && pwd)}"

if [ -z "$ROS_DISTRO" ]; then
  echo "Error: ROS2 not sourced. Run: source /opt/ros/${ROS_DISTRO:-humble}/setup.bash"
  exit 1
fi

export LD_LIBRARY_PATH=$BUILD_DIR:$LD_LIBRARY_PATH

echo "=========================================="
echo "ROS2 Interoperability Test"
echo "ROS2 Distribution: $ROS_DISTRO"
echo "=========================================="

# Start Ibmw publisher
echo "Starting Ibmw publisher..."
$BUILD_DIR/plain_pub_only_app $CFG_DIR/plain_pub_only_cfg.yaml &
IBMW_PID=$!
sleep 2

# Listen with ROS2
echo "Listening with ROS2 subscriber (10 seconds)..."
timeout 10 ros2 topic echo /rt/point_topic geometry_msgs/msg/Point || true

# Cleanup
kill $IBMW_PID 2>/dev/null || true
wait $IBMW_PID 2>/dev/null || true

echo ""
echo "=========================================="
echo "ROS2 Interop Test Complete"
echo "If Point messages appeared above, the test passed."
echo "=========================================="
