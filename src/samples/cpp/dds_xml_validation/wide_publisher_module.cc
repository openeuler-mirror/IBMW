// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "wide_publisher_module.h"

#include <chrono>
#include <iostream>

#include "internal/typesupport/ros2/ros2_transport.h"

namespace ibmw::samples::dds_xml_wide {

using namespace std::chrono_literals;

bool PublisherModule::Passed() const {
  return registrations_ok_.load() && matrix_sent_.load() && service_ok_.load();
}

uint32_t PublisherModule::SentTopicCount() const {
  return registrations_ok_.load() && matrix_sent_.load() ? 13 : 0;
}

void PublisherModule::RequestShutdown() {
  bool expected = false;
  if (shutdown_requested_.compare_exchange_strong(expected, true) && shutdown_) {
    shutdown_();
  }
}

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;
  auto transport = core_.GetTransport();

#define REGISTER_SENDER(field, type, topic)                    \
  field = transport.GetPublisher(topic);                       \
  if (!ibmw::ros2::transport::RegisterSendType<type>(field)) { \
    return false;                                              \
  }

  REGISTER_SENDER(image_, sensor_msgs::msg::Image, kImageTopic)
  REGISTER_SENDER(compressed_, sensor_msgs::msg::CompressedImage,
                  kCompressedImageTopic)
  REGISTER_SENDER(imu_, sensor_msgs::msg::Imu, kImuTopic)
  REGISTER_SENDER(scan_, sensor_msgs::msg::LaserScan, kLaserScanTopic)
  REGISTER_SENDER(cloud_, sensor_msgs::msg::PointCloud2, kPointCloudTopic)
  REGISTER_SENDER(camera_, sensor_msgs::msg::CameraInfo, kCameraInfoTopic)
  REGISTER_SENDER(odom_, nav_msgs::msg::Odometry, kOdometryTopic)
  REGISTER_SENDER(path_, nav_msgs::msg::Path, kPathTopic)
  REGISTER_SENDER(grid_, nav_msgs::msg::OccupancyGrid, kOccupancyGridTopic)
  REGISTER_SENDER(metadata_, nav_msgs::msg::MapMetaData, kMapMetaDataTopic)
  REGISTER_SENDER(tf_, tf2_msgs::msg::TFMessage, kTfTopic)
  REGISTER_SENDER(marker_, visualization_msgs::msg::MarkerArray, kMarkerTopic)
  REGISTER_SENDER(diagnostic_, diagnostic_msgs::msg::DiagnosticArray,
                  kDiagnosticTopic)

#undef REGISTER_SENDER

  registrations_ok_ = true;
  auto service = core_.GetService();
  return slam_toolbox::srv::RegisterSerializePoseGraphClientFunc(service);
}

bool PublisherModule::Run() {
  running_ = true;
  thread_ = std::thread(&PublisherModule::Loop, this);
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
  std::cout << "IBMW_XML_WIDE_RESULT role=publisher status="
            << (Passed() ? "PASS" : "FAIL")
            << " topics=" << SentTopicCount()
            << " service=" << (service_ok_ ? "PASS" : "FAIL") << std::endl;
}

void PublisherModule::Loop() {
  auto access =
      slam_toolbox::srv::SerializePoseGraphSyncAccess(core_.GetService());
  const auto deadline = std::chrono::steady_clock::now() + 20s;

  while (running_ && !service_ok_ &&
         std::chrono::steady_clock::now() < deadline) {
    ibmw::ros2::transport::Send(image_, Image());
    ibmw::ros2::transport::Send(compressed_, CompressedImage());
    ibmw::ros2::transport::Send(imu_, Imu());
    ibmw::ros2::transport::Send(scan_, LaserScan());
    ibmw::ros2::transport::Send(cloud_, PointCloud2());
    ibmw::ros2::transport::Send(camera_, CameraInfo());
    ibmw::ros2::transport::Send(odom_, Odometry());
    ibmw::ros2::transport::Send(path_, Path());
    ibmw::ros2::transport::Send(grid_, OccupancyGrid());
    ibmw::ros2::transport::Send(metadata_, MapMetaData());
    ibmw::ros2::transport::Send(tf_, TFMessage());
    ibmw::ros2::transport::Send(marker_, MarkerArray());
    ibmw::ros2::transport::Send(diagnostic_, DiagnosticArray());
    matrix_sent_ = true;

    slam_toolbox::srv::SerializePoseGraph_Request request;
    slam_toolbox::srv::SerializePoseGraph_Response response;
    request.filename = "ibmw_xml_pose_graph";
    auto context = access.NewContextSharedPtr();
    context->SetTimeout(3s);
    const auto status = access.SerializePoseGraph(context, request, response);
    if (status.OK() &&
        response.result ==
            slam_toolbox::srv::SerializePoseGraph_Response::RESULT_SUCCESS) {
      service_ok_ = true;
      RequestShutdown();
      return;
    }
    std::this_thread::sleep_for(100ms);
  }

  RequestShutdown();
}

}  // namespace ibmw::samples::dds_xml_wide
