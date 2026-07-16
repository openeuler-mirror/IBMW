// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "subscriber_module.h"
#include <cmath>

namespace ibmw::examples::loaned_message_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  {
    auto subscriber = core_.GetTransport().GetSubscriber("point_topic");
    bool success = ibmw::ros2::transport::OnMessage<geometry_msgs::msg::Point>(
        subscriber, "geometry_msgs/msg/Point",
        std::bind(&SubscriberModule::OnPointMessage, this, std::placeholders::_1));

    if (!success) {
      MW_ERROR("Failed to subscribe Point");
      return false;
    }
    MW_INFO("Subscribed to point_topic");
  }

  {
    auto subscriber = core_.GetTransport().GetSubscriber("string_topic");
    bool success = ibmw::ros2::transport::OnMessage<std_msgs::msg::String>(
        subscriber, "std_msgs/msg/String",
        std::bind(&SubscriberModule::OnStringMessage, this, std::placeholders::_1));

    if (!success) {
      MW_ERROR("Failed to subscribe String");
      return false;
    }
    MW_INFO("Subscribed to string_topic");
  }

  MW_INFO("SubscriberModule initialized");
  return true;
}

bool SubscriberModule::Run() {
  MW_INFO("SubscriberModule started");
  return true;
}

void SubscriberModule::Stop() {
  MW_INFO("SubscriberModule shutdown - Point received: {}, String received: {}",
             point_count_.load(), string_count_.load());
}

void SubscriberModule::OnPointMessage(
    const std::shared_ptr<const geometry_msgs::msg::Point>& msg) {
  uint64_t count = ++point_count_;

  // Validate data is reasonable (no NaN, finite values)
  bool data_ok = std::isfinite(msg->x) && std::isfinite(msg->y) && std::isfinite(msg->z);

  if (count % 5 == 0 || count <= 3) {
    MW_INFO("[RECV] Point[{}] x={:.2f}, y={:.2f}, z={:.2f}, valid={}, ptr={}",
               count, msg->x, msg->y, msg->z,
               data_ok ? "YES" : "NO",
               static_cast<const void*>(msg.get()));
  }

  if (!data_ok) {
    MW_ERROR("[RECV] Point[{}] contains invalid data!", count);
  }
}

void SubscriberModule::OnStringMessage(
    const std::shared_ptr<const std_msgs::msg::String>& msg) {
  uint64_t count = ++string_count_;

  bool data_ok = !msg->data.empty();

  if (count % 5 == 0 || count <= 3) {
    MW_INFO("[RECV] String[{}] data=\"{}\", valid={}, ptr={}",
               count, msg->data,
               data_ok ? "YES" : "NO",
               static_cast<const void*>(msg.get()));
  }
}

}  // namespace ibmw::examples::loaned_message_demo
