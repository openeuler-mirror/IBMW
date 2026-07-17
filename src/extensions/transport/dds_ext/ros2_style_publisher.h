// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// IBMW is licensed under Mulan PSL v2.

#pragma once

#include <memory>
#include <string>

#include "loaned_message.h"
#include "loaned_message_impl.h"

#include <fastdds/dds/publisher/DataWriter.hpp>

namespace ibmw::extensions::dds_extension {

/**
 * @brief ROS2-style Publisher class
 *
 * Provides an interface compatible with ROS2 rclcpp::Publisher:
 * - borrow_loaned_message() - borrow a zero-copy message
 * - publish() - publish a message
 */
template<typename MsgType>
class Ros2StylePublisher {
 public:
  Ros2StylePublisher(
      eprosima::fastdds::dds::DataWriter* writer,
      const std::string& topic_name,
      bool is_plain,
      const ZeroCopyConfig& config = ZeroCopyConfig::Auto())
      : writer_(writer),
        topic_name_(topic_name),
        is_plain_(is_plain),
        config_(config) {}

  ~Ros2StylePublisher() = default;

  Ros2StylePublisher(const Ros2StylePublisher&) = delete;
  Ros2StylePublisher& operator=(const Ros2StylePublisher&) = delete;

  Ros2StylePublisher(Ros2StylePublisher&&) = default;
  Ros2StylePublisher& operator=(Ros2StylePublisher&&) = default;

  LoanedMessage<MsgType> borrow_loaned_message() {
    return BorrowLoanedMessage<MsgType>(writer_, topic_name_, is_plain_, config_);
  }

  bool publish(LoanedMessage<MsgType>&& loaned_msg) {
    return SendLoaned(std::move(loaned_msg));
  }

  bool publish(const MsgType& msg) {
    if (writer_ == nullptr) {
      return false;
    }
    return writer_->write(const_cast<MsgType*>(&msg));
  }

  bool publish(MsgType&& msg) {
    if (writer_ == nullptr) {
      return false;
    }
    return writer_->write(&msg);
  }

  const std::string& get_topic_name() const {
    return topic_name_;
  }

  bool can_loan_messages() const {
    return CanLoanMessages<MsgType>(writer_, is_plain_);
  }

  eprosima::fastdds::dds::DataWriter* get_writer() const {
    return writer_;
  }

  const ZeroCopyConfig& get_config() const {
    return config_;
  }

  void set_config(const ZeroCopyConfig& config) {
    config_ = config;
  }

 private:
  eprosima::fastdds::dds::DataWriter* writer_;
  std::string topic_name_;
  bool is_plain_;
  ZeroCopyConfig config_;
};

template<typename MsgType>
using Ros2StylePublisherPtr = std::shared_ptr<Ros2StylePublisher<MsgType>>;

}  // namespace ibmw::extensions::dds_extension
