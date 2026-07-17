// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_ext/ros2_adapter_subscription.h"
#include "cpp/util/type_support.h"
#include "ros2_ext/global.h"

namespace ibmw::extensions::ros2_extension {
std::shared_ptr<void> Ros2AdapterSubscription::create_message() {
  return message_handler_.info.msg_type_support_ref.CreateSharedPtr();
}

std::shared_ptr<rclcpp::SerializedMessage>
Ros2AdapterSubscription::create_serialized_message() {
  return std::make_shared<rclcpp::SerializedMessage>();
}

void Ros2AdapterSubscription::handle_message(
    std::shared_ptr<void>& message, const rclcpp::MessageInfo& message_info) {
  if (!run_flag_.load()) return;

  try {
    auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

    ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(message_info.get_rmw_message_info().source_timestamp));
    ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ, std::to_string(message_info.get_rmw_message_info().publication_sequence_number));

    msg_handler_tool_.DoMessageCallback(ctx_ptr, message_handler_, message);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void Ros2AdapterSubscription::handle_serialized_message(
    const std::shared_ptr<rclcpp::SerializedMessage>& serialized_message,
    const rclcpp::MessageInfo& message_info) {
  if (!run_flag_.load()) return;

  try {
    auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);
    ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP, std::to_string(message_info.get_rmw_message_info().source_timestamp));
    ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ, std::to_string(message_info.get_rmw_message_info().publication_sequence_number));

    const auto serialization_type = std::string(message_handler_.info.msg_type_support_ref.DefaultSerializationType());
    const auto& rcl_ser = serialized_message->get_rcl_serialized_message();

    msg_handler_tool_.DoMessageCallback(
        ctx_ptr, serialization_type, static_cast<const void*>(rcl_ser.buffer), rcl_ser.buffer_length);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void Ros2AdapterSubscription::handle_loaned_message(
    void* loaned_message, const rclcpp::MessageInfo& message_info) {
  MW_WARN("not support ros2 loaned message");
}

void Ros2AdapterSubscription::return_message(std::shared_ptr<void>& message) {
  message.reset();
}

void Ros2AdapterSubscription::return_serialized_message(
    std::shared_ptr<rclcpp::SerializedMessage>& message) {
  message.reset();
}

}  // namespace ibmw::extensions::ros2_extension
