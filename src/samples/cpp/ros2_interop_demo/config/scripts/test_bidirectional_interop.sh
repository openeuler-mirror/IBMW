#!/bin/bash
# Rigorous ROS2-Ibmw Bidirectional Interop Test
# Test 1: Ibmw publishes -> ROS2 demo_nodes_cpp listener receives
# Test 2: ROS2 demo_nodes_cpp talker publishes -> Ibmw receives

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "ROS2-Ibmw Bidirectional Interop Test"
echo "=========================================="

# Source ROS2 environment
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash

# Check environment first
echo ""
echo "Running environment check..."
./check_environment.sh
if [ $? -ne 0 ]; then
    echo -e "${RED}❌ Environment check failed!${NC}"
    exit 1
fi

echo ""
echo "=========================================="
echo "TEST 1: Ibmw Publisher -> ROS2 Listener"
echo "=========================================="
echo ""
echo "Starting Ibmw publisher (pub only, no internal subscriber)..."

# Start Ibmw publisher only
./ros2_pub_only_app ./cfg/ros2_pub_only_cfg.yaml > /tmp/ibmw_pub.log 2>&1 &
IBMW_PUB_PID=$!

# Wait for initialization
sleep 3

# Check if Ibmw is still running
if ! ps -p $IBMW_PUB_PID > /dev/null; then
    echo -e "${RED}❌ Ibmw publisher failed to start!${NC}"
    cat /tmp/ibmw_pub.log
    exit 1
fi

echo "Ibmw publisher started (PID: $IBMW_PUB_PID)"
echo ""

# Check topic discovery
echo "Checking ROS2 topic discovery..."
sleep 2
TOPICS=$(ros2 topic list)
if echo "$TOPICS" | grep -q "/chatter"; then
    echo -e "${GREEN}✅ /chatter topic discovered by ROS2${NC}"
else
    echo -e "${RED}❌ /chatter topic NOT found!${NC}"
    echo "Available topics:"
    echo "$TOPICS"
    kill $IBMW_PUB_PID
    exit 1
fi

# Check topic info
echo ""
echo "Topic info:"
ros2 topic info /chatter

# Start ROS2 listener
echo ""
echo "Starting ROS2 demo_nodes_cpp listener..."
timeout 10 ros2 run demo_nodes_cpp listener > /tmp/ros2_listener.log 2>&1 &
LISTENER_PID=$!

sleep 8

# Check listener output
echo ""
echo "Checking ROS2 listener output..."
if grep -q "I heard:" /tmp/ros2_listener.log; then
    echo -e "${GREEN}✅ TEST 1 PASSED: ROS2 listener received messages from Ibmw!${NC}"
    echo ""
    echo "Sample messages received:"
    grep "I heard:" /tmp/ros2_listener.log | head -5
else
    echo -e "${RED}❌ TEST 1 FAILED: ROS2 listener did not receive messages${NC}"
    echo ""
    echo "Listener log:"
    cat /tmp/ros2_listener.log
    kill $IBMW_PUB_PID 2>/dev/null || true
    exit 1
fi

# Cleanup test 1
kill $IBMW_PUB_PID 2>/dev/null || true
wait $LISTENER_PID 2>/dev/null || true

# Wait for cleanup
sleep 3

echo ""
echo "=========================================="
echo "TEST 2: ROS2 Talker -> Ibmw Subscriber"
echo "=========================================="
echo ""
echo "Starting Ibmw subscriber (sub only, no internal publisher)..."

# Start Ibmw subscriber only
./ros2_sub_only_app ./cfg/ros2_sub_only_cfg.yaml > /tmp/ibmw_sub.log 2>&1 &
IBMW_SUB_PID=$!

# Wait for initialization
sleep 3

# Check if Ibmw is still running
if ! ps -p $IBMW_SUB_PID > /dev/null; then
    echo -e "${RED}❌ Ibmw subscriber failed to start!${NC}"
    cat /tmp/ibmw_sub.log
    exit 1
fi

echo "Ibmw subscriber started (PID: $IBMW_SUB_PID)"
echo ""

# Start ROS2 talker
echo "Starting ROS2 demo_nodes_cpp talker..."
timeout 10 ros2 run demo_nodes_cpp talker > /tmp/ros2_talker.log 2>&1 &
TALKER_PID=$!

sleep 8

# Check Ibmw subscriber output
echo ""
echo "Checking Ibmw subscriber output..."
if grep -q "Received" /tmp/ibmw_sub.log; then
    echo -e "${GREEN}✅ TEST 2 PASSED: Ibmw subscriber received messages from ROS2!${NC}"
    echo ""
    echo "Sample messages received:"
    grep "Received" /tmp/ibmw_sub.log | head -5
else
    echo -e "${RED}❌ TEST 2 FAILED: Ibmw subscriber did not receive messages${NC}"
    echo ""
    echo "Ibmw subscriber log:"
    cat /tmp/ibmw_sub.log
    echo ""
    echo "ROS2 talker log:"
    cat /tmp/ros2_talker.log
    kill $IBMW_SUB_PID 2>/dev/null || true
    exit 1
fi

# Cleanup test 2
kill $IBMW_SUB_PID 2>/dev/null || true
wait $TALKER_PID 2>/dev/null || true

echo ""
echo "=========================================="
echo "🎉 ALL TESTS PASSED!"
echo "=========================================="
echo ""
echo "Summary:"
echo -e "${GREEN}✅ TEST 1: Ibmw Publisher -> ROS2 Listener${NC}"
echo -e "${GREEN}✅ TEST 2: ROS2 Talker -> Ibmw Subscriber${NC}"
echo ""
echo "Bidirectional ROS2-Ibmw interop verified successfully!"
echo ""
