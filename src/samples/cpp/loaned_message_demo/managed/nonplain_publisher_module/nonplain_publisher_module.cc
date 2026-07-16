// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "nonplain_publisher_module.h"

namespace ibmw::examples::loaned_message_demo {

bool NonPlainPublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_INFO("NonPlainPublisherModule initializing...");

  publisher_ = core_.GetTransport().GetPublisher("string_topic");

  bool success = ibmw::ros2::transport::RegisterSendType<std_msgs::msg::String>(
      publisher_, "std_msgs/msg/String");

  if (!success) {
    MW_ERROR("Failed to register String type");
    return false;
  }

  MW_INFO("String type size: {} bytes", sizeof(std_msgs::msg::String));
  MW_INFO("String is_trivially_copyable: {}",
             std::is_trivially_copyable_v<std_msgs::msg::String>);
  MW_INFO("NonPlainPublisherModule initialized");
  return true;
}

bool NonPlainPublisherModule::Run() {
  if (driver_ != nullptr) {
    loaned_publisher_ =
        ibmw::extensions::dds_extension::create_publisher_with_driver_config<
            std_msgs::msg::String>(driver_, "string_topic", "std_msgs/msg/String");

    if (!loaned_publisher_) {
      MW_WARN("Failed to create loaned publisher for string_topic");
    } else {
      MW_INFO("Loaned publisher created for string_topic");
      MW_INFO("  can_loan_messages: {}", loaned_publisher_->can_loan_messages());
    }
  } else {
    MW_WARN("DDS Driver not set");
  }

  running_ = true;
  publish_thread_ = std::thread([this]() { PublishLoop(); });
  MW_INFO("NonPlainPublisherModule started");
  return true;
}

void NonPlainPublisherModule::Stop() {
  running_ = false;

  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }

  PrintStatistics();
  MW_INFO("NonPlainPublisherModule shutdown - Total published: {}", publish_count_);
}

void NonPlainPublisherModule::PublishLoop() {
  if (!loaned_publisher_) {
    MW_WARN("Loaned publisher not available, using traditional API");
    // Fallback to traditional API
    while (running_) {
      std_msgs::msg::String msg;
      msg.data = "Hello LoanedMessage #" + std::to_string(publish_count_);

      if (publish_count_ % 5 == 0) {
        MW_INFO("[SEND] String[{}] data=\"{}\", is_plain=false (traditional API)",
                   publish_count_, msg.data);
      }

      ibmw::ros2::transport::Send<std_msgs::msg::String>(
          publisher_, "std_msgs/msg/String", msg);
      publish_count_++;
      fallback_count_++;

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return;
  }

  // Use LoanedMessage API to verify fallback behavior, then publish via traditional API
  while (running_) {
    // Step 1: Verify LoanedMessage API behavior for non-Plain type
    auto loaned_msg = loaned_publisher_->borrow_loaned_message();

    if (!loaned_msg.is_valid()) {
      // Expected in STRICT mode for non-Plain types
      invalid_count_++;

      if (publish_count_ % 5 == 0) {
        MW_INFO("[SEND] String[{}] borrow rejected (STRICT mode): {}",
                   publish_count_, loaned_msg.error_message());
      }
    } else {
      // AUTO/COMPAT mode: valid but not zero-copy
      if (loaned_msg.is_zero_copy()) {
        MW_WARN("Unexpected: non-Plain type got zero-copy!");
      }
      fallback_count_++;

      if (publish_count_ % 5 == 0) {
        MW_INFO("[SEND] String[{}] borrow ok: is_zero_copy={}, is_plain={}, is_valid=true",
                   publish_count_, loaned_msg.is_zero_copy(), loaned_msg.is_plain_type());
      }
      // Let loaned_msg go out of scope - don't publish via DataWriter for non-Plain
    }

    // Step 2: Actual publish via traditional API (proper serialization)
    std_msgs::msg::String msg;
    msg.data = "Hello LoanedMessage #" + std::to_string(publish_count_);

    ibmw::ros2::transport::Send<std_msgs::msg::String>(
        publisher_, "std_msgs/msg/String", msg);
    publish_count_++;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void NonPlainPublisherModule::PrintStatistics() {
  MW_INFO("========== Statistics (NonPlain) ==========");
  MW_INFO("Total published: {}", publish_count_);
  MW_INFO("Fallback (heap): {}", fallback_count_);
  MW_INFO("Invalid (STRICT rejected): {}", invalid_count_);
  MW_INFO("===========================================");
}

}  // namespace ibmw::examples::loaned_message_demo
