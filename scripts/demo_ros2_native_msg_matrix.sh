#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_ROOT="${WORK_ROOT:-${TMPDIR:-/tmp}/ibmw-ros2-native-msg-matrix-demo}"
IBMW_PREFIX="${IBMW_PREFIX:-${TMPDIR:-/tmp}/ibmw-msggen-validation/ibmw-host-install-phase0}"
ROS2_SETUP="${ROS2_SETUP:-/opt/ros/humble/setup.bash}"
ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"

require_file() {
  local path=$1
  [[ -f "${path}" ]] || {
    echo "ERROR: required file not found: ${path}" >&2
    exit 1
  }
}

require_dir() {
  local path=$1
  [[ -d "${path}" ]] || {
    echo "ERROR: required directory not found: ${path}" >&2
    exit 1
  }
}

require_file "${ROS2_SETUP}"
require_dir "${IBMW_PREFIX}"
require_file "${ROOT_DIR}/scripts/live_ros2_ibmw_interop_acceptance.sh"

set +u
# shellcheck disable=SC1090
source "${ROS2_SETUP}"
set -u

export ROS_DOMAIN_ID
export LD_LIBRARY_PATH="${IBMW_PREFIX}/lib:/opt/ros/humble/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/opt/ros/humble/lib/python3.10/site-packages:/opt/ros/humble/local/lib/python3.10/dist-packages${PYTHONPATH:+:${PYTHONPATH}}"
export AMENT_PREFIX_PATH="/opt/ros/humble${AMENT_PREFIX_PATH:+:${AMENT_PREFIX_PATH}}"
export CMAKE_PREFIX_PATH="${IBMW_PREFIX}:/opt/ros/humble${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

rm -rf "${WORK_ROOT}"
mkdir -p "${WORK_ROOT}"
MATRIX_CASES_TSV="${WORK_ROOT}/msg_live_matrix_cases.tsv"
: >"${MATRIX_CASES_TSV}"

write_yaml() {
  local case_id=$1
  local cpp_type=$2
  local include=$3
  local ros2_type=$4
  local packages=$5
  local topic_name=$6
  local yaml_path=$7

  cat > "${yaml_path}" <<EOF
schema_version: 2
project:
  name: ${case_id}_native_msg_demo
  namespace: ${case_id}_native_msg_demo

messages:
  ${cpp_type}:
    wire:
      ros2:
        include: "${include}"
        type: "${ros2_type}"
        packages: [${packages}]

dds_driver:
  domain_id: ${ROS_DOMAIN_ID}
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"

modules:
  - name: NativePublisherModule
    type: direct_ros2_publisher
    topic: "${topic_name#/}"
    message: "${cpp_type}"
  - name: NativeSubscriberModule
    type: direct_ros2_subscriber
    topic: "${topic_name#/}"
    message: "${cpp_type}"

apps:
  - name: ros2_pub_only_app
    modules: [NativePublisherModule]
  - name: ros2_sub_only_app
    modules: [NativeSubscriberModule]
EOF
}

run_case() {
  local case_id=$1
  local cpp_type=$2
  local include=$3
  local ros2_type=$4
  local packages=$5
  local topic_name=$6
  local ros2_echo_expect=$7
  local ros2_payload=$8

  local case_dir="${WORK_ROOT}/${case_id}"
  local yaml_path="${case_dir}/${case_id}.ibmw.yaml"
  local gen_dir="${case_dir}/gen"
  local build_dir="${case_dir}/build"
  local log_dir="${case_dir}/logs"

  mkdir -p "${case_dir}"
  write_yaml "${case_id}" "${cpp_type}" "${include}" "${ros2_type}" "${packages}" "${topic_name}" "${yaml_path}"

  python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" "${yaml_path}" -o "${gen_dir}" --force
  cmake -S "${gen_dir}" -B "${build_dir}" -DPython3_EXECUTABLE=/usr/bin/python3
  cmake --build "${build_dir}" -j"$(nproc)"
  cp "${IBMW_PREFIX}/lib/libibmw_dds_transport.so" "${build_dir}/"

  "${ROOT_DIR}/scripts/live_ros2_ibmw_interop_acceptance.sh" pubsub \
    --workdir "${build_dir}" \
    --log-dir "${log_dir}" \
    --pubsub-topic "${topic_name}" \
    --pubsub-type "${ros2_type}" \
    --ros2-sub-cmd "ros2 topic echo --once ${topic_name} ${ros2_type}" \
    --ros2-pub-cmd "ros2 topic pub --times 10 --rate 2 ${topic_name} ${ros2_type} '${ros2_payload}'" \
    --ros2-sub-expect "${ros2_echo_expect}" \
    --ibmw-sub-expect "Received ROS2 interop message"

  printf '%s\t%s\t%s\t%s\t%s\n' "${case_id}" "${ros2_type}" "${cpp_type}" "${packages}" "${topic_name}" >>"${MATRIX_CASES_TSV}"
}

run_case \
  "pose_stamped" \
  "geometry_msgs::msg::PoseStamped" \
  "geometry_msgs/msg/pose_stamped.hpp" \
  "geometry_msgs/msg/PoseStamped" \
  "geometry_msgs" \
  "/ibmw_pose_stamped" \
  "pose:" \
  "{header: {frame_id: map}, pose: {position: {x: 1.0, y: 2.0, z: 3.0}, orientation: {w: 1.0}}}"

run_case \
  "imu" \
  "sensor_msgs::msg::Imu" \
  "sensor_msgs/msg/imu.hpp" \
  "sensor_msgs/msg/Imu" \
  "sensor_msgs" \
  "/ibmw_imu" \
  "orientation:" \
  "{header: {frame_id: imu}, orientation: {w: 1.0}, angular_velocity: {x: 0.1, y: 0.2, z: 0.3}, linear_acceleration: {z: 9.8}}"

run_case \
  "path" \
  "nav_msgs::msg::Path" \
  "nav_msgs/msg/path.hpp" \
  "nav_msgs/msg/Path" \
  "nav_msgs" \
  "/ibmw_path" \
  "poses:" \
  "{header: {frame_id: map}, poses: [{header: {frame_id: map}, pose: {position: {x: 1.0, y: 2.0, z: 0.0}, orientation: {w: 1.0}}}]}"

run_case \
  "point_cloud2" \
  "sensor_msgs::msg::PointCloud2" \
  "sensor_msgs/msg/point_cloud2.hpp" \
  "sensor_msgs/msg/PointCloud2" \
  "sensor_msgs" \
  "/ibmw_points" \
  "fields:" \
  "{header: {frame_id: map}, height: 1, width: 2, fields: [{name: x, offset: 0, datatype: 7, count: 1}], is_bigendian: false, point_step: 4, row_step: 8, data: [0, 0, 128, 63, 0, 0, 0, 64], is_dense: true}"

run_case "std_bool" "std_msgs::msg::Bool" "std_msgs/msg/bool.hpp" "std_msgs/msg/Bool" "std_msgs" "/ibmw_std_bool" "data:" "{data: true}"
run_case "std_int32" "std_msgs::msg::Int32" "std_msgs/msg/int32.hpp" "std_msgs/msg/Int32" "std_msgs" "/ibmw_std_int32" "data:" "{data: 42}"
run_case "std_float64" "std_msgs::msg::Float64" "std_msgs/msg/float64.hpp" "std_msgs/msg/Float64" "std_msgs" "/ibmw_std_float64" "data:" "{data: 3.14}"
run_case "std_string" "std_msgs::msg::String" "std_msgs/msg/string.hpp" "std_msgs/msg/String" "std_msgs" "/ibmw_std_string" "data:" "{data: hello}"
run_case "std_header" "std_msgs::msg::Header" "std_msgs/msg/header.hpp" "std_msgs/msg/Header" "std_msgs" "/ibmw_std_header" "frame_id:" "{stamp: {sec: 1, nanosec: 2}, frame_id: map}"
run_case "std_color_rgba" "std_msgs::msg::ColorRGBA" "std_msgs/msg/color_rgba.hpp" "std_msgs/msg/ColorRGBA" "std_msgs" "/ibmw_std_color_rgba" "r:" "{r: 0.1, g: 0.2, b: 0.3, a: 1.0}"
run_case "std_uint8_multi_array" "std_msgs::msg::UInt8MultiArray" "std_msgs/msg/u_int8_multi_array.hpp" "std_msgs/msg/UInt8MultiArray" "std_msgs" "/ibmw_std_uint8_multi_array" "data:" "{layout: {dim: [{label: bytes, size: 3, stride: 3}], data_offset: 0}, data: [1, 2, 3]}"

run_case "builtin_time" "builtin_interfaces::msg::Time" "builtin_interfaces/msg/time.hpp" "builtin_interfaces/msg/Time" "builtin_interfaces" "/ibmw_builtin_time" "sec:" "{sec: 7, nanosec: 42}"
run_case "builtin_duration" "builtin_interfaces::msg::Duration" "builtin_interfaces/msg/duration.hpp" "builtin_interfaces/msg/Duration" "builtin_interfaces" "/ibmw_builtin_duration" "sec:" "{sec: 8, nanosec: 9}"

run_case "geom_point" "geometry_msgs::msg::Point" "geometry_msgs/msg/point.hpp" "geometry_msgs/msg/Point" "geometry_msgs" "/ibmw_geom_point" "x:" "{x: 1.0, y: 2.0, z: 3.0}"
run_case "geom_quaternion" "geometry_msgs::msg::Quaternion" "geometry_msgs/msg/quaternion.hpp" "geometry_msgs/msg/Quaternion" "geometry_msgs" "/ibmw_geom_quaternion" "w:" "{x: 0.0, y: 0.0, z: 0.0, w: 1.0}"
run_case "geom_vector3" "geometry_msgs::msg::Vector3" "geometry_msgs/msg/vector3.hpp" "geometry_msgs/msg/Vector3" "geometry_msgs" "/ibmw_geom_vector3" "x:" "{x: 0.1, y: 0.2, z: 0.3}"
run_case "geom_pose" "geometry_msgs::msg::Pose" "geometry_msgs/msg/pose.hpp" "geometry_msgs/msg/Pose" "geometry_msgs" "/ibmw_geom_pose" "position:" "{position: {x: 1.0, y: 2.0, z: 3.0}, orientation: {w: 1.0}}"
run_case "geom_twist" "geometry_msgs::msg::Twist" "geometry_msgs/msg/twist.hpp" "geometry_msgs/msg/Twist" "geometry_msgs" "/ibmw_geom_twist" "linear:" "{linear: {x: 0.5}, angular: {z: 0.1}}"
run_case "geom_wrench" "geometry_msgs::msg::Wrench" "geometry_msgs/msg/wrench.hpp" "geometry_msgs/msg/Wrench" "geometry_msgs" "/ibmw_geom_wrench" "force:" "{force: {x: 1.0}, torque: {z: 2.0}}"
run_case "geom_transform" "geometry_msgs::msg::Transform" "geometry_msgs/msg/transform.hpp" "geometry_msgs/msg/Transform" "geometry_msgs" "/ibmw_geom_transform" "translation:" "{translation: {x: 1.0}, rotation: {w: 1.0}}"
run_case "geom_pose_array" "geometry_msgs::msg::PoseArray" "geometry_msgs/msg/pose_array.hpp" "geometry_msgs/msg/PoseArray" "geometry_msgs" "/ibmw_geom_pose_array" "poses:" "{header: {frame_id: map}, poses: [{position: {x: 1.0}, orientation: {w: 1.0}}]}"
run_case "geom_polygon_stamped" "geometry_msgs::msg::PolygonStamped" "geometry_msgs/msg/polygon_stamped.hpp" "geometry_msgs/msg/PolygonStamped" "geometry_msgs" "/ibmw_geom_polygon_stamped" "polygon:" "{header: {frame_id: map}, polygon: {points: [{x: 1.0, y: 2.0, z: 0.0}]}}"
run_case "geom_twist_cov_stamped" "geometry_msgs::msg::TwistWithCovarianceStamped" "geometry_msgs/msg/twist_with_covariance_stamped.hpp" "geometry_msgs/msg/TwistWithCovarianceStamped" "geometry_msgs" "/ibmw_geom_twist_cov_stamped" "twist:" "{header: {frame_id: base}, twist: {twist: {linear: {x: 0.5}, angular: {z: 0.1}}}}"
run_case "geom_accel_cov_stamped" "geometry_msgs::msg::AccelWithCovarianceStamped" "geometry_msgs/msg/accel_with_covariance_stamped.hpp" "geometry_msgs/msg/AccelWithCovarianceStamped" "geometry_msgs" "/ibmw_geom_accel_cov_stamped" "accel:" "{header: {frame_id: base}, accel: {accel: {linear: {z: 9.8}}}}"

run_case "sensor_image" "sensor_msgs::msg::Image" "sensor_msgs/msg/image.hpp" "sensor_msgs/msg/Image" "sensor_msgs" "/ibmw_sensor_image" "encoding:" "{header: {frame_id: camera}, height: 1, width: 2, encoding: rgb8, is_bigendian: false, step: 6, data: [255, 0, 0, 0, 255, 0]}"
run_case "sensor_camera_info" "sensor_msgs::msg::CameraInfo" "sensor_msgs/msg/camera_info.hpp" "sensor_msgs/msg/CameraInfo" "sensor_msgs" "/ibmw_sensor_camera_info" "height:" "{header: {frame_id: camera}, height: 480, width: 640, distortion_model: plumb_bob}"
run_case "sensor_laser_scan" "sensor_msgs::msg::LaserScan" "sensor_msgs/msg/laser_scan.hpp" "sensor_msgs/msg/LaserScan" "sensor_msgs" "/ibmw_sensor_laser_scan" "ranges:" "{header: {frame_id: laser}, angle_min: 0.0, angle_max: 1.0, angle_increment: 1.0, time_increment: 0.0, scan_time: 0.1, range_min: 0.1, range_max: 10.0, ranges: [1.0], intensities: [0.5]}"
run_case "sensor_joint_state" "sensor_msgs::msg::JointState" "sensor_msgs/msg/joint_state.hpp" "sensor_msgs/msg/JointState" "sensor_msgs" "/ibmw_sensor_joint_state" "name:" "{header: {frame_id: base}, name: [joint1], position: [1.0], velocity: [0.0], effort: [0.0]}"
run_case "sensor_nav_sat_fix" "sensor_msgs::msg::NavSatFix" "sensor_msgs/msg/nav_sat_fix.hpp" "sensor_msgs/msg/NavSatFix" "sensor_msgs" "/ibmw_sensor_nav_sat_fix" "latitude:" "{header: {frame_id: gps}, status: {status: 0, service: 1}, latitude: 1.0, longitude: 2.0, altitude: 3.0}"
run_case "sensor_battery_state" "sensor_msgs::msg::BatteryState" "sensor_msgs/msg/battery_state.hpp" "sensor_msgs/msg/BatteryState" "sensor_msgs" "/ibmw_sensor_battery_state" "voltage:" "{header: {frame_id: battery}, voltage: 12.0, current: 1.0, percentage: 0.5}"
run_case "sensor_magnetic_field" "sensor_msgs::msg::MagneticField" "sensor_msgs/msg/magnetic_field.hpp" "sensor_msgs/msg/MagneticField" "sensor_msgs" "/ibmw_sensor_magnetic_field" "magnetic_field:" "{header: {frame_id: imu}, magnetic_field: {x: 1.0, y: 0.0, z: 0.0}}"
run_case "sensor_range" "sensor_msgs::msg::Range" "sensor_msgs/msg/range.hpp" "sensor_msgs/msg/Range" "sensor_msgs" "/ibmw_sensor_range" "range:" "{header: {frame_id: ranger}, radiation_type: 0, field_of_view: 0.5, min_range: 0.1, max_range: 10.0, range: 1.0}"
run_case "sensor_temperature" "sensor_msgs::msg::Temperature" "sensor_msgs/msg/temperature.hpp" "sensor_msgs/msg/Temperature" "sensor_msgs" "/ibmw_sensor_temperature" "temperature:" "{header: {frame_id: temp}, temperature: 23.5, variance: 0.1}"
run_case "sensor_fluid_pressure" "sensor_msgs::msg::FluidPressure" "sensor_msgs/msg/fluid_pressure.hpp" "sensor_msgs/msg/FluidPressure" "sensor_msgs" "/ibmw_sensor_fluid_pressure" "fluid_pressure:" "{header: {frame_id: pressure}, fluid_pressure: 101325.0, variance: 1.0}"
run_case "sensor_illuminance" "sensor_msgs::msg::Illuminance" "sensor_msgs/msg/illuminance.hpp" "sensor_msgs/msg/Illuminance" "sensor_msgs" "/ibmw_sensor_illuminance" "illuminance:" "{header: {frame_id: light}, illuminance: 100.0, variance: 0.5}"
run_case "sensor_joy" "sensor_msgs::msg::Joy" "sensor_msgs/msg/joy.hpp" "sensor_msgs/msg/Joy" "sensor_msgs" "/ibmw_sensor_joy" "axes:" "{header: {frame_id: joy}, axes: [0.1, 0.2], buttons: [1, 0]}"
run_case "sensor_compressed_image" "sensor_msgs::msg::CompressedImage" "sensor_msgs/msg/compressed_image.hpp" "sensor_msgs/msg/CompressedImage" "sensor_msgs" "/ibmw_sensor_compressed_image" "format:" "{header: {frame_id: camera}, format: jpeg, data: [1, 2, 3]}"

run_case "nav_odometry" "nav_msgs::msg::Odometry" "nav_msgs/msg/odometry.hpp" "nav_msgs/msg/Odometry" "nav_msgs" "/ibmw_nav_odometry" "child_frame_id:" "{header: {frame_id: odom}, child_frame_id: base_link, pose: {pose: {orientation: {w: 1.0}}}, twist: {twist: {linear: {x: 0.5}}}}"
run_case "nav_map_metadata" "nav_msgs::msg::MapMetaData" "nav_msgs/msg/map_meta_data.hpp" "nav_msgs/msg/MapMetaData" "nav_msgs" "/ibmw_nav_map_metadata" "resolution:" "{map_load_time: {sec: 1}, resolution: 0.05, width: 2, height: 2, origin: {orientation: {w: 1.0}}}"
run_case "nav_occupancy_grid" "nav_msgs::msg::OccupancyGrid" "nav_msgs/msg/occupancy_grid.hpp" "nav_msgs/msg/OccupancyGrid" "nav_msgs" "/ibmw_nav_occupancy_grid" "data:" "{header: {frame_id: map}, info: {resolution: 0.05, width: 2, height: 2, origin: {orientation: {w: 1.0}}}, data: [0, 100, -1, 50]}"

python3 - "${WORK_ROOT}" "${MATRIX_CASES_TSV}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
tsv = pathlib.Path(sys.argv[2])
cases = []
for line in tsv.read_text(encoding="utf-8").splitlines():
    case_id, ros2_type, cpp_type, packages, topic_name = line.split("\t")
    cases.append(
        {
            "case": case_id,
            "ros2_type": ros2_type,
            "cpp_type": cpp_type,
            "packages": packages.split(", ") if packages else [],
            "topic": topic_name,
            "ibmw_to_ros2_pass": True,
            "ros2_to_ibmw_pass": True,
            "status": "pass",
        }
    )
summary = {
    "schema_version": 1,
    "artifact": "msg_live_matrix_summary",
    "lane": "msg",
    "uses_rcl_in_ibmw_runtime": False,
    "uses_ros2_cli_as_import_prerequisite": False,
    "cases": cases,
    "totals": {"selected": len(cases), "pass": len(cases)},
}
path = work / "msg_live_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] msg live matrix summary: {path}")
PY

echo "PASS: ROS2 native .msg matrix demo completed: 40 runtime cases (${WORK_ROOT})"
