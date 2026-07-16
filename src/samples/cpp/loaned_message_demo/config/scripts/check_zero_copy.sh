#!/bin/bash
# check_zero_copy.sh - 检查零拷贝环境配置
set -e

BUILD_DIR="${BUILD_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../build" 2>/dev/null && pwd)}"

echo "=========================================="
echo "Zero-Copy Configuration Check"
echo "=========================================="

echo ""
echo "1. Shared Memory:"
if [ -d "/dev/shm" ]; then
  echo "  /dev/shm exists"
  df -h /dev/shm | tail -1
else
  echo "  /dev/shm NOT found"
fi

echo ""
echo "2. DDS Plugin:"
if [ -f "$BUILD_DIR/libibmw_dds_plugin.so" ]; then
  echo "  libibmw_dds_plugin.so found"
  ls -lh "$BUILD_DIR/libibmw_dds_plugin.so" | awk '{print "  Size:", $5, "Date:", $6, $7, $8}'
else
  echo "  libibmw_dds_plugin.so NOT found"
fi

echo ""
echo "3. Demo Executables:"
for app in loaned_message_app config_test_app plain_pub_only_app nonplain_pub_only_app sub_only_app; do
  if [ -f "$BUILD_DIR/$app" ]; then
    echo "  [OK] $app"
  else
    echo "  [MISSING] $app"
  fi
done

echo ""
echo "4. Config Files:"
CFG_DIR="$(dirname "${BASH_SOURCE[0]}")/../cfg"
for cfg in loaned_message_cfg.yaml strict_mode_cfg.yaml compat_mode_cfg.yaml config_test_cfg.yaml plain_pub_only_cfg.yaml nonplain_pub_only_cfg.yaml sub_only_cfg.yaml; do
  if [ -f "$CFG_DIR/$cfg" ]; then
    echo "  [OK] $cfg"
  else
    echo "  [MISSING] $cfg"
  fi
done

echo ""
echo "5. ROS2 Environment:"
if [ -n "$ROS_DISTRO" ]; then
  echo "  ROS_DISTRO: $ROS_DISTRO"
  echo "  RMW: ${RMW_IMPLEMENTATION:-default}"
else
  echo "  ROS2 not sourced"
fi

echo ""
echo "=========================================="
echo "Check Complete"
echo "=========================================="
