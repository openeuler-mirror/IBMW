// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "plain_publisher_module.h"

namespace ibmw::examples::loaned_message_demo {

bool PlainPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_INFO("PlainPublisherModule initializing...");

  auto publisher = core_.GetTransport().GetPublisher("point_topic");

  bool success = ibmw::ros2::transport::RegisterSendType<geometry_msgs::msg::Point>(
      publisher, "geometry_msgs/msg/Point");

  if (!success) {
    MW_ERROR("Failed to register Point type");
    return false;
  }

  MW_INFO("Point type size: {} bytes", sizeof(geometry_msgs::msg::Point));
  MW_INFO("Point is_trivially_copyable: {}", std::is_trivially_copyable_v<geometry_msgs::msg::Point>);
  MW_INFO("Point is_standard_layout: {}", std::is_standard_layout_v<geometry_msgs::msg::Point>);
  MW_INFO("PlainPublisherModule initialized");
  return true;
}

bool PlainPublisherModule::Run() {
  if (driver_ != nullptr) {
    loaned_publisher_ = ibmw::extensions::dds_extension::create_publisher_with_driver_config<
        geometry_msgs::msg::Point>(driver_, "point_topic", "geometry_msgs/msg/Point");

    if (!loaned_publisher_) {
      MW_ERROR("Failed to create loaned publisher");
    } else {
      MW_INFO("Loaned publisher created for point_topic");
      MW_INFO("  is_plain: {}", driver_->IsPlainType(loaned_publisher_->get_topic_name()));
      MW_INFO("  can_loan_messages: {}", loaned_publisher_->can_loan_messages());
    }
  } else {
    MW_WARN("DDS Driver not set, cannot use LoanedMessage API");
  }

  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("PlainPublisherModule started");
  return true;
}

void PlainPublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  PrintStatistics();
  MW_INFO("PlainPublisherModule shutdown - Total published: {}", publish_count_);
}

void PlainPublisherModule::PublishLoop() {
  if (!loaned_publisher_) {
    MW_ERROR("Loaned publisher not initialized, cannot publish");
    return;
  }

  while (running_) {
    auto loaned_msg = loaned_publisher_->borrow_loaned_message();

    if (!loaned_msg.is_valid()) {
      MW_ERROR("Failed to borrow loaned message: {}", loaned_msg.error_message());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (loaned_msg.is_zero_copy()) {
      zero_copy_count_++;
    } else {
      fallback_count_++;
    }

    double expected_x = 1.0 + publish_count_ * 0.1;
    double expected_y = 2.0 + publish_count_ * 0.2;
    double expected_z = 3.0 + publish_count_ * 0.3;

    loaned_msg->x = expected_x;
    loaned_msg->y = expected_y;
    loaned_msg->z = expected_z;

    if (publish_count_ % 5 == 0) {
      void* msg_ptr = loaned_msg.GetRawPointer();
      MW_INFO("[SEND] Point[{}] x={:.2f}, y={:.2f}, z={:.2f}, "
                 "is_zero_copy={}, is_plain={}, ptr={}",
                 publish_count_,
                 expected_x, expected_y, expected_z,
                 loaned_msg.is_zero_copy(),
                 loaned_msg.is_plain_type(),
                 msg_ptr);
    }

    bool success = loaned_publisher_->publish(std::move(loaned_msg));

    if (success) {
      publish_count_++;
    } else {
      MW_ERROR("Failed to publish loaned message");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void PlainPublisherModule::PrintStatistics() {
  uint64_t total = zero_copy_count_ + fallback_count_;
  double ratio = total > 0 ? (double)zero_copy_count_ / total * 100.0 : 0.0;

  MW_INFO("========== Zero-Copy Statistics (Plain) ==========");
  MW_INFO("Total published: {}", publish_count_);
  MW_INFO("Zero-copy: {} ({:.1f}%)", zero_copy_count_, ratio);
  MW_INFO("Fallback: {}", fallback_count_);

  if (driver_) {
    std::string dds_topic = loaned_publisher_ ? loaned_publisher_->get_topic_name() : "";
    const auto* stats = driver_->GetZeroCopyStats(dds_topic);
    if (stats) {
      MW_INFO("Driver loan_success: {}", stats->loan_success.load());
      MW_INFO("Driver loan_failure: {}", stats->loan_failure.load());
      MW_INFO("Driver zero_copy_publish: {}", stats->zero_copy_publish.load());
      MW_INFO("Driver fallback_publish: {}", stats->fallback_publish.load());
    }
  }
  MW_INFO("==================================================");
}

}  // namespace ibmw::examples::loaned_message_demo
