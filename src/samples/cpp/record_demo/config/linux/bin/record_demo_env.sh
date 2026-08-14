#!/usr/bin/env bash

# Source this file from every terminal that runs record_demo or its bag CLI.
RECORD_DEMO_HAD_NOUNSET=0
case "$-" in
  *u*) RECORD_DEMO_HAD_NOUNSET=1; set +u ;;
  *) ;;
esac
RECORD_DEMO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f /opt/ros/humble/setup.bash ]]; then
  echo "record_demo: /opt/ros/humble/setup.bash is missing; install ROS2 Humble first." >&2
  if [[ "${RECORD_DEMO_HAD_NOUNSET}" == 1 ]]; then
    set -u
  fi
  unset RECORD_DEMO_DIR RECORD_DEMO_HAD_NOUNSET
  return 1 2>/dev/null || exit 1
fi
source /opt/ros/humble/setup.bash

RECORD_DEMO_ROS2_PREFIX="${RECORD_DEMO_DIR}/record_demo_ros2_runtime"
RECORD_DEMO_PACKAGE_MARKER="${RECORD_DEMO_ROS2_PREFIX}/share/ament_index/resource_index/packages/example_ros2"
if [[ ! -f "${RECORD_DEMO_PACKAGE_MARKER}" ]] ||
   [[ ! -f "${RECORD_DEMO_ROS2_PREFIX}/lib/libexample_ros2__rosidl_typesupport_cpp.so" ]]; then
  echo "record_demo: private ROS2 interface prefix is incomplete: ${RECORD_DEMO_ROS2_PREFIX}" >&2
  echo "record_demo: build the ibmw_samples_cpp_record_demo_build_all target first." >&2
  unset RECORD_DEMO_PACKAGE_MARKER RECORD_DEMO_ROS2_PREFIX RECORD_DEMO_DIR
  if [[ "${RECORD_DEMO_HAD_NOUNSET}" == 1 ]]; then
    set -u
  fi
  unset RECORD_DEMO_HAD_NOUNSET
  return 1 2>/dev/null || exit 1
fi
export AMENT_PREFIX_PATH="${RECORD_DEMO_ROS2_PREFIX}:${AMENT_PREFIX_PATH:-}"
export LD_LIBRARY_PATH="${RECORD_DEMO_ROS2_PREFIX}/lib:${RECORD_DEMO_DIR}:${RECORD_DEMO_DIR}/lib:/opt/ros/humble/lib:${LD_LIBRARY_PATH:-}"
unset RECORD_DEMO_PACKAGE_MARKER RECORD_DEMO_ROS2_PREFIX
unset RECORD_DEMO_DIR
if [[ "${RECORD_DEMO_HAD_NOUNSET}" == 1 ]]; then
  set -u
fi
unset RECORD_DEMO_HAD_NOUNSET
