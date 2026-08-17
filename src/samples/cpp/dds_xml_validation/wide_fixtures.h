// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

#include "diagnostic_msgs/msg/diagnostic_array.hpp"
#include "geometry_msgs/msg/pose_stamped.hpp"
#include "std_msgs/msg/header.hpp"
#include "nav_msgs/msg/map_meta_data.hpp"
#include "nav_msgs/msg/occupancy_grid.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "nav_msgs/msg/path.hpp"
#include "sensor_msgs/msg/camera_info.hpp"
#include "sensor_msgs/msg/compressed_image.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "sensor_msgs/msg/imu.hpp"
#include "sensor_msgs/msg/laser_scan.hpp"
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/point_field.hpp"
#include "std_msgs/msg/color_rgba.hpp"
#include "tf2_msgs/msg/tf_message.hpp"
#include "geometry_msgs/msg/transform_stamped.hpp"
#include "visualization_msgs/msg/marker.hpp"
#include "visualization_msgs/msg/marker_array.hpp"

#include "diagnostic_msgs/msg/detail/diagnostic_array__rosidl_typesupport_fastrtps_cpp.hpp"
#include "nav_msgs/msg/detail/map_meta_data__rosidl_typesupport_fastrtps_cpp.hpp"
#include "nav_msgs/msg/detail/occupancy_grid__rosidl_typesupport_fastrtps_cpp.hpp"
#include "nav_msgs/msg/detail/odometry__rosidl_typesupport_fastrtps_cpp.hpp"
#include "nav_msgs/msg/detail/path__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/camera_info__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/compressed_image__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/image__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/imu__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/laser_scan__rosidl_typesupport_fastrtps_cpp.hpp"
#include "sensor_msgs/msg/detail/point_cloud2__rosidl_typesupport_fastrtps_cpp.hpp"
#include "tf2_msgs/msg/detail/tf_message__rosidl_typesupport_fastrtps_cpp.hpp"
#include "visualization_msgs/msg/detail/marker_array__rosidl_typesupport_fastrtps_cpp.hpp"

namespace ibmw::samples::dds_xml_wide {

inline constexpr char kImageTopic[] = "ibmw/xml/wide/image";
inline constexpr char kCompressedImageTopic[] = "ibmw/xml/wide/compressed_image";
inline constexpr char kImuTopic[] = "ibmw/xml/wide/imu";
inline constexpr char kLaserScanTopic[] = "ibmw/xml/wide/laser_scan";
inline constexpr char kPointCloudTopic[] = "ibmw/xml/wide/point_cloud";
inline constexpr char kCameraInfoTopic[] = "ibmw/xml/wide/camera_info";
inline constexpr char kOdometryTopic[] = "ibmw/xml/wide/odometry";
inline constexpr char kPathTopic[] = "ibmw/xml/wide/path";
inline constexpr char kOccupancyGridTopic[] = "ibmw/xml/wide/occupancy_grid";
inline constexpr char kMapMetaDataTopic[] = "ibmw/xml/wide/map_metadata";
inline constexpr char kTfTopic[] = "ibmw/xml/wide/tf";
inline constexpr char kMarkerTopic[] = "ibmw/xml/wide/markers";
inline constexpr char kDiagnosticTopic[] = "ibmw/xml/wide/diagnostics";

inline std_msgs::msg::Header Header(const char* frame) {
  std_msgs::msg::Header h;
  h.frame_id = frame;
  h.stamp.sec = 71;
  h.stamp.nanosec = 123456789;
  return h;
}

inline sensor_msgs::msg::Image Image() {
  sensor_msgs::msg::Image m;
  m.header = Header("camera");
  m.height = 2;
  m.width = 3;
  m.encoding = "rgb8";
  m.is_bigendian = 0;
  m.step = 9;
  m.data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
  return m;
}

inline sensor_msgs::msg::CompressedImage CompressedImage() {
  sensor_msgs::msg::CompressedImage m;
  m.header = Header("camera");
  m.format = "jpeg";
  m.data = {9, 8, 7, 6, 5, 4};
  return m;
}

inline sensor_msgs::msg::Imu Imu() {
  sensor_msgs::msg::Imu m;
  m.header = Header("imu");
  m.orientation.x = .1;
  m.orientation.y = .2;
  m.orientation.z = .3;
  m.orientation.w = .9;
  m.angular_velocity.x = 1.1;
  m.angular_velocity.y = 1.2;
  m.angular_velocity.z = 1.3;
  m.linear_acceleration.x = 2.1;
  m.linear_acceleration.y = 2.2;
  m.linear_acceleration.z = 2.3;
  m.orientation_covariance[0] = .01;
  m.angular_velocity_covariance[4] = .02;
  m.linear_acceleration_covariance[8] = .03;
  return m;
}

inline sensor_msgs::msg::LaserScan LaserScan() {
  sensor_msgs::msg::LaserScan m;
  m.header = Header("laser");
  m.angle_min = -.5;
  m.angle_max = .5;
  m.angle_increment = .25;
  m.time_increment = .01;
  m.scan_time = .1;
  m.range_min = .2;
  m.range_max = 30.;
  m.ranges = {1., 2., 3., 4., 5.};
  m.intensities = {10., 20., 30., 40., 50.};
  return m;
}

inline sensor_msgs::msg::PointCloud2 PointCloud2() {
  sensor_msgs::msg::PointCloud2 m;
  m.header = Header("lidar");
  m.height = 1;
  m.width = 2;
  m.is_bigendian = false;
  sensor_msgs::msg::PointField field;
  field.name = "x";
  field.offset = 0;
  field.datatype = 7;
  field.count = 1;
  m.fields = {field};
  m.point_step = 4;
  m.row_step = 8;
  m.data = {0, 0, 128, 63, 0, 0, 0, 64};
  m.is_dense = true;
  return m;
}

inline sensor_msgs::msg::CameraInfo CameraInfo() {
  sensor_msgs::msg::CameraInfo m;
  m.header = Header("camera");
  m.height = 480;
  m.width = 640;
  m.distortion_model = "plumb_bob";
  m.d = {.1, .2, .3, .4, .5};
  m.k[0] = 500.;
  m.k[4] = 500.;
  m.k[2] = 320.;
  m.k[5] = 240.;
  m.k[8] = 1.;
  m.r[0] = m.r[4] = m.r[8] = 1.;
  m.p[0] = 500.;
  m.p[5] = 500.;
  m.p[2] = 320.;
  m.p[6] = 240.;
  m.binning_x = 1;
  m.binning_y = 1;
  m.roi.x_offset = 2;
  m.roi.y_offset = 3;
  m.roi.height = 10;
  m.roi.width = 11;
  return m;
}

inline nav_msgs::msg::Odometry Odometry() {
  nav_msgs::msg::Odometry m;
  m.header = Header("odom");
  m.child_frame_id = "base_link";
  m.pose.pose.position.x = 1.;
  m.pose.pose.position.y = 2.;
  m.pose.pose.orientation.w = 1.;
  m.twist.twist.linear.x = .5;
  m.twist.twist.angular.z = .25;
  m.pose.covariance[0] = .1;
  m.twist.covariance[0] = .2;
  return m;
}

inline nav_msgs::msg::Path Path() {
  nav_msgs::msg::Path m;
  m.header = Header("map");
  geometry_msgs::msg::PoseStamped pose;
  pose.header = Header("map");
  pose.pose.position.x = 3.;
  pose.pose.position.y = 4.;
  pose.pose.orientation.w = 1.;
  m.poses = {pose};
  return m;
}

inline nav_msgs::msg::MapMetaData MapMetaData() {
  nav_msgs::msg::MapMetaData m;
  m.map_load_time.sec = 71;
  m.map_load_time.nanosec = 9;
  m.resolution = .05;
  m.width = 4;
  m.height = 3;
  m.origin.position.x = -1.;
  m.origin.position.y = -2.;
  m.origin.orientation.w = 1.;
  return m;
}

inline nav_msgs::msg::OccupancyGrid OccupancyGrid() {
  nav_msgs::msg::OccupancyGrid m;
  m.header = Header("map");
  m.info = MapMetaData();
  m.data = {0, 100, -1, 50, 25, 75, 1, 2, 3, 4, 5, 6};
  return m;
}

inline tf2_msgs::msg::TFMessage TFMessage() {
  tf2_msgs::msg::TFMessage m;
  geometry_msgs::msg::TransformStamped transform;
  transform.header = Header("map");
  transform.child_frame_id = "base_link";
  transform.transform.translation.x = 1.;
  transform.transform.rotation.w = 1.;
  m.transforms = {transform};
  return m;
}

inline visualization_msgs::msg::MarkerArray MarkerArray() {
  visualization_msgs::msg::MarkerArray m;
  visualization_msgs::msg::Marker marker;
  marker.header = Header("map");
  marker.ns = "ibmw";
  marker.id = 71;
  marker.type = visualization_msgs::msg::Marker::CUBE;
  marker.action = visualization_msgs::msg::Marker::ADD;
  marker.pose.orientation.w = 1.;
  marker.scale.x = 1.;
  marker.scale.y = 2.;
  marker.scale.z = 3.;
  marker.color.r = 1.;
  marker.color.g = .5;
  marker.color.a = 1.;
  marker.text = "wide";
  m.markers = {marker};
  return m;
}

inline diagnostic_msgs::msg::DiagnosticArray DiagnosticArray() {
  diagnostic_msgs::msg::DiagnosticArray m;
  m.header = Header("base_link");
  diagnostic_msgs::msg::DiagnosticStatus status;
  status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
  status.name = "ibmw";
  status.message = "wide fixture";
  status.hardware_id = "xml-71";
  diagnostic_msgs::msg::KeyValue value;
  value.key = "fixture";
  value.value = "13";
  status.values = {value};
  m.status = {status};
  return m;
}

}  // namespace ibmw::samples::dds_xml_wide
