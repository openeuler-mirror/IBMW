// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "wide_subscriber_module.h"

#include <chrono>
#include <iostream>

#include "type_support/ros2/ros2_transport.h"

namespace ibmw::samples::dds_xml_wide {
namespace {

constexpr uint16_t kAllTopicsMask = (1u << 13) - 1;

class SerializePoseGraphServiceImpl final
    : public slam_toolbox::srv::SerializePoseGraphSyncService {
 public:
  explicit SerializePoseGraphServiceImpl(SubscriberModule& owner)
      : owner_(owner) {}

  ibmw::rpc::Status SerializePoseGraph(
      ibmw::rpc::ContextRef,
      const slam_toolbox::srv::SerializePoseGraph_Request& request,
      slam_toolbox::srv::SerializePoseGraph_Response& response) override {
    const bool ready = request.filename == "ibmw_xml_pose_graph" &&
                       owner_.TopicsComplete();
    response.result =
        ready
            ? slam_toolbox::srv::SerializePoseGraph_Response::RESULT_SUCCESS
            : slam_toolbox::srv::SerializePoseGraph_Response::
                  RESULT_FAILED_TO_WRITE_FILE;
    owner_.OnServiceRequest(ready);
    // Native ROS2 .srv replies carry application failure in the response.
    // Returning an IBMW RPC error would suppress the DDS reply and prevent a
    // stock ROS2 client from observing the result and retrying.
    return ibmw::rpc::Status();
  }

 private:
  SubscriberModule& owner_;
};

}  // namespace

bool SubscriberModule::Passed() const {
  return TopicsComplete() && service_ok_.load();
}

bool SubscriberModule::TopicsComplete() const {
  return mask_.load() == kAllTopicsMask;
}

uint32_t SubscriberModule::ReceivedTopicCount() const {
  uint16_t mask = mask_.load();
  uint32_t count = 0;
  while (mask != 0) {
    count += mask & 1u;
    mask >>= 1;
  }
  return count;
}

void SubscriberModule::OnServiceRequest(bool valid) {
  if (valid) {
    service_ok_ = true;
  }
}

void SubscriberModule::RequestShutdown() {
  bool expected = false;
  if (shutdown_requested_.compare_exchange_strong(expected, true) && shutdown_) {
    shutdown_();
  }
}

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

#define ON_MESSAGE(topic, type, fixture, bit)                              \
  if (!ibmw::ros2::transport::OnMessage<type>(                            \
          core_.GetTransport().GetSubscriber(topic),                      \
          [this](const std::shared_ptr<const type>& message) {             \
            if (message && *message == fixture()) {                       \
              mask_.fetch_or(1u << bit);                                  \
            }                                                             \
          })) {                                                           \
    return false;                                                         \
  }

  ON_MESSAGE(kImageTopic, sensor_msgs::msg::Image, Image, 0)
  ON_MESSAGE(kCompressedImageTopic, sensor_msgs::msg::CompressedImage,
             CompressedImage, 1)
  ON_MESSAGE(kImuTopic, sensor_msgs::msg::Imu, Imu, 2)
  ON_MESSAGE(kLaserScanTopic, sensor_msgs::msg::LaserScan, LaserScan, 3)
  ON_MESSAGE(kPointCloudTopic, sensor_msgs::msg::PointCloud2, PointCloud2, 4)
  ON_MESSAGE(kCameraInfoTopic, sensor_msgs::msg::CameraInfo, CameraInfo, 5)
  ON_MESSAGE(kOdometryTopic, nav_msgs::msg::Odometry, Odometry, 6)
  ON_MESSAGE(kPathTopic, nav_msgs::msg::Path, Path, 7)
  ON_MESSAGE(kOccupancyGridTopic, nav_msgs::msg::OccupancyGrid, OccupancyGrid,
             8)
  ON_MESSAGE(kMapMetaDataTopic, nav_msgs::msg::MapMetaData, MapMetaData, 9)
  ON_MESSAGE(kTfTopic, tf2_msgs::msg::TFMessage, TFMessage, 10)
  ON_MESSAGE(kMarkerTopic, visualization_msgs::msg::MarkerArray, MarkerArray,
             11)
  ON_MESSAGE(kDiagnosticTopic, diagnostic_msgs::msg::DiagnosticArray,
             DiagnosticArray, 12)

#undef ON_MESSAGE

  service_ = std::make_shared<SerializePoseGraphServiceImpl>(*this);
  return core_.GetService().RegisterService(service_.get());
}

bool SubscriberModule::Run() {
  running_ = true;
  timeout_thread_ = std::thread([this] {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (running_ && std::chrono::steady_clock::now() < deadline) {
      if (Passed()) {
        // Let the service adapter publish and acknowledge its response before
        // this validation process tears down the DDS endpoints.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (running_) {
          RequestShutdown();
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (running_) {
      RequestShutdown();
    }
  });
  return true;
}

void SubscriberModule::Stop() {
  running_ = false;
  if (timeout_thread_.joinable()) {
    timeout_thread_.join();
  }
  std::cout << "IBMW_XML_WIDE_RESULT role=subscriber status="
            << (Passed() ? "PASS" : "FAIL")
            << " topics=" << ReceivedTopicCount()
            << " service=" << (service_ok_ ? "PASS" : "FAIL") << std::endl;
}

}  // namespace ibmw::samples::dds_xml_wide
