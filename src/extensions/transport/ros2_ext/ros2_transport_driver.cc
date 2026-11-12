// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_ext/ros2_transport_driver.h"

#include <regex>
#include <optional>
#include <string_view>

#include "cpp/util/string.h"
#include "cpp/util/type_support.h"
#include "ros2_ext/global.h"
#include "ros2_ext/ros2_name_encode.h"
#include "ros2_ext/detail/qos_observation.h"

#include "rcl/error_handling.h"
#include "rclcpp/version.h"
#include "rclcpp/typesupport_helpers.hpp"
#include "rmw/serialized_message.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::ros2_extension::Ros2TransportDriver::Options> {
  using Options = ibmw::extensions::ros2_extension::Ros2TransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["use_serialized"] = pub_topic_options.use_serialized;
      pub_topic_options_node["qos"]["history"] = pub_topic_options.qos.history;
      pub_topic_options_node["qos"]["depth"] = pub_topic_options.qos.depth;
      pub_topic_options_node["qos"]["reliability"] = pub_topic_options.qos.reliability;
      pub_topic_options_node["qos"]["durability"] = pub_topic_options.qos.durability;
      pub_topic_options_node["qos"]["lifespan"] = pub_topic_options.qos.lifespan;
      pub_topic_options_node["qos"]["deadline"] = pub_topic_options.qos.deadline;
      pub_topic_options_node["qos"]["liveliness"] = pub_topic_options.qos.liveliness;
      pub_topic_options_node["qos"]["liveliness_lease_duration"] = pub_topic_options.qos.liveliness_lease_duration;
      node["publishers"].push_back(pub_topic_options_node);
    }

    node["subscribers"] = YAML::Node();
    for (const auto& sub_topic_options : rhs.sub_topics_options) {
      Node sub_topic_options_node;
      sub_topic_options_node["topic"] = sub_topic_options.topic_name;
      sub_topic_options_node["use_serialized"] = sub_topic_options.use_serialized;
      sub_topic_options_node["qos"]["history"] = sub_topic_options.qos.history;
      sub_topic_options_node["qos"]["depth"] = sub_topic_options.qos.depth;
      sub_topic_options_node["qos"]["reliability"] = sub_topic_options.qos.reliability;
      sub_topic_options_node["qos"]["durability"] = sub_topic_options.qos.durability;
      sub_topic_options_node["qos"]["lifespan"] = sub_topic_options.qos.lifespan;
      sub_topic_options_node["qos"]["deadline"] = sub_topic_options.qos.deadline;
      sub_topic_options_node["qos"]["liveliness"] = sub_topic_options.qos.liveliness;
      sub_topic_options_node["qos"]["liveliness_lease_duration"] = sub_topic_options.qos.liveliness_lease_duration;
      node["subscribers"].push_back(sub_topic_options_node);
    }

    return node;
  }

  static bool decodeQos(const Node& node, Options::QosOptions& qos) {
    if (node) {
      if (node["history"]) {
        qos.history = node["history"].as<std::string>();
      }
      if (node["depth"]) {
        qos.depth = node["depth"].as<int>();
      }
      if (node["reliability"]) {
        qos.reliability = node["reliability"].as<std::string>();
      }
      if (node["durability"]) {
        qos.durability = node["durability"].as<std::string>();
      }
      if (node["lifespan"]) {
        qos.lifespan = node["lifespan"].as<int>();
      }
      if (node["deadline"]) {
        qos.deadline = node["deadline"].as<int>();
      }
      if (node["liveliness"]) {
        qos.liveliness = node["liveliness"].as<std::string>();
      }
      if (node["liveliness_lease_duration"]) {
        qos.liveliness_lease_duration = node["liveliness_lease_duration"].as<int>();
      }
    }
    return true;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& pub_topic_options_node : node["publishers"]) {
        auto pub_topic_options = Options::PubTopicOptions{
            .topic_name = pub_topic_options_node["topic"].as<std::string>()};
        if (pub_topic_options_node["qos"]) {
          decodeQos(pub_topic_options_node["qos"], pub_topic_options.qos);
        }
        if (pub_topic_options_node["use_serialized"]) {
          pub_topic_options.use_serialized = pub_topic_options_node["use_serialized"].as<bool>();
        }
        rhs.pub_topics_options.emplace_back(std::move(pub_topic_options));
      }
    }

    if (node["subscribers"] && node["subscribers"].IsSequence()) {
      for (const auto& sub_topic_options_node : node["subscribers"]) {
        auto sub_topic_options = Options::SubTopicOptions{
            .topic_name = sub_topic_options_node["topic"].as<std::string>()};
        if (sub_topic_options_node["use_serialized"]) {
          sub_topic_options.use_serialized = sub_topic_options_node["use_serialized"].as<bool>();
        }
        if (sub_topic_options_node["qos"]) {
          decodeQos(sub_topic_options_node["qos"], sub_topic_options.qos);
        }
        rhs.sub_topics_options.emplace_back(std::move(sub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::ros2_extension {

namespace {

struct Ros2TypeSupportHandle {
  std::shared_ptr<rcpputils::SharedLibrary> library;
  const rosidl_message_type_support_t* handle = nullptr;
};

Ros2TypeSupportHandle LoadRos2TypeSupport(std::string_view ibmw_type_name) {
  MW_ASSERT(
      ibmw_type_name.starts_with("ros2:") && ibmw_type_name.size() > 5,
      "Invalid ROS2 message type '{}'.", ibmw_type_name);
  const std::string ros2_type_name(ibmw_type_name.substr(5));
  auto library = rclcpp::get_typesupport_library(
      ros2_type_name, "rosidl_typesupport_cpp");
  MW_ASSERT(
      library != nullptr, "Cannot load ROS2 TypeSupport library for '{}'.",
      ros2_type_name);
  const auto* handle = rclcpp::get_typesupport_handle(
      ros2_type_name, "rosidl_typesupport_cpp", *library);
  MW_ASSERT(
      handle != nullptr, "Cannot load ROS2 TypeSupport handle for '{}'.",
      ros2_type_name);
  return {.library = std::move(library), .handle = handle};
}

void ApplyQosDescriptor(
    const runtime::core::transport::QosDescriptor& descriptor,
    Ros2TransportDriver::Options::QosOptions& qos) {
  const auto apply_string = [&](std::string_view name, std::string& value) {
    const auto found = descriptor.policies.find(std::string(name));
    if (found != descriptor.policies.end()) {
      value = found->second;
    }
  };
  const auto apply_int32 = [&](std::string_view name, int32_t& value) {
    const auto found = descriptor.policies.find(std::string(name));
    if (found != descriptor.policies.end()) {
      value = std::stoi(found->second);
    }
  };
  const auto apply_duration = [&descriptor](
                                  std::string_view canonical_name,
                                  std::string_view legacy_name,
                                  int64_t& value) {
    auto found = descriptor.policies.find(std::string(canonical_name));
    if (found == descriptor.policies.end()) {
      found = descriptor.policies.find(std::string(legacy_name));
    }
    if (found != descriptor.policies.end()) {
      value = std::stoll(found->second);
    }
  };
  apply_string("history", qos.history);
  apply_int32("depth", qos.depth);
  apply_string("reliability", qos.reliability);
  apply_string("durability", qos.durability);
  apply_string("liveliness", qos.liveliness);
  apply_duration("deadline_ms", "deadline", qos.deadline);
  apply_duration("lifespan_ms", "lifespan", qos.lifespan);
  apply_duration(
      "liveliness_lease_duration_ms", "liveliness_lease_duration",
      qos.liveliness_lease_duration);
}

void ApplyQosProfileDescriptor(
    const runtime::core::transport::QosDescriptor& descriptor,
    rclcpp::QoS& qos) {
  const auto apply_duration = [&descriptor](
                                  std::string_view name,
                                  const auto& setter) {
    const auto found = descriptor.policies.find(std::string(name));
    if (found == descriptor.policies.end()) {
      return;
    }
    const int64_t nanoseconds = std::stoll(found->second);
    MW_ASSERT(
        nanoseconds >= 0, "ROS2 QoS duration '{}' cannot be negative.",
        name);
    setter(rclcpp::Duration::from_nanoseconds(nanoseconds));
  };
  apply_duration(
      "deadline_ns", [&qos](const auto& value) { qos.deadline(value); });
  apply_duration(
      "lifespan_ns", [&qos](const auto& value) { qos.lifespan(value); });
  apply_duration(
      "liveliness_lease_duration_ns",
      [&qos](const auto& value) {
        qos.liveliness_lease_duration(value);
      });
  const auto avoid =
      descriptor.policies.find("avoid_ros_namespace_conventions");
  if (avoid != descriptor.policies.end()) {
    MW_ASSERT(
        avoid->second == "true" || avoid->second == "false" ||
            avoid->second == "1" || avoid->second == "0",
        "avoid_ros_namespace_conventions must be true or false.");
    qos.avoid_ros_namespace_conventions(
        avoid->second == "true" || avoid->second == "1");
  }
}

}  // namespace

void Ros2TransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Ros2 transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void Ros2TransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (auto& itr : ros2_message_handler_map_) {
    static_cast<Ros2AdapterSubscription*>(itr.second.ros_sub_handle_ptr.get())->Start();
  }
}

void Ros2TransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  std::vector<uint64_t> handles;
  {
    std::lock_guard lock(native_mutex_);
    for (const auto& [handle, subscriber] : native_subscribers_) handles.push_back(handle);
  }
  for (const auto handle : handles) DetachSerializedSubscriber({handle});

  for (auto& itr : ros2_send_type_wrapper_map_) {
    rcl_publisher_t& publisher = *(itr.second.publisher_ptr);
    rcl_ret_t ret = rcl_publisher_fini(
        &publisher,
        ros2_node_ptr_->get_node_base_interface()->get_shared_rcl_node_handle().get());
    if (ret != RMW_RET_OK) {
      MW_WARN(
          "Publisher fini failed in ros2 transport driver, type '{}', error info: {}",
          itr.first.msg_type, rcl_get_error_string().str);
      rcl_reset_error();
    }
  }

  for (auto& itr : ros2_message_handler_map_) {
    static_cast<Ros2AdapterSubscription*>(itr.second.ros_sub_handle_ptr.get())->Shutdown();
  }

  ros2_node_ptr_.reset();
}

runtime::core::transport::SerializedTransportOperationsV2
Ros2TransportDriver::SerializedOperations() noexcept {
  using namespace runtime::core::transport;
  return {.context = this,
          .supports_topic = [](void*, const TopicDescriptor& topic) noexcept {
            return topic.msg_type.starts_with("ros2:") && topic.serialization_type == "ros2";
          },
          .attach_subscriber = [](void* driver, const SerializedSubscriberOptionsV2& options,
                                  SerializedMessageCallback callback) noexcept {
            return static_cast<Ros2TransportDriver*>(driver)->AttachSerializedSubscriber(options, std::move(callback));
          },
          .detach_subscriber = [](void* driver, SerializedSubscriberHandle handle) noexcept {
            return static_cast<Ros2TransportDriver*>(driver)->DetachSerializedSubscriber(handle);
          },
          .publisher_qos = [](void* driver, std::string_view topic, std::string_view type) noexcept {
            return static_cast<Ros2TransportDriver*>(driver)->EndpointQos(topic, type, true);
          },
          .subscriber_qos = [](void* driver, std::string_view topic, std::string_view type) noexcept {
            return static_cast<Ros2TransportDriver*>(driver)->EndpointQos(topic, type, false);
          }};
}

ibmw::transport::QosDescriptor Ros2TransportDriver::EndpointQos(
    std::string_view topic, std::string_view msg_type, bool publisher) const noexcept {
  try {
    if (!ros2_node_ptr_) return {};
    std::optional<rmw_qos_profile_t> qos;
    std::string wire_topic;
    if (msg_type.starts_with("ros2:")) {
      const Key key{std::string(topic), std::string(msg_type)};
      if (publisher) {
        const auto it = ros2_send_type_wrapper_map_.find(key);
        if (it != ros2_send_type_wrapper_map_.end()) {
          if (const auto* actual = rcl_publisher_get_actual_qos(it->second.publisher_ptr.get())) qos = *actual;
          if (const auto* name = rcl_publisher_get_topic_name(it->second.publisher_ptr.get())) wire_topic = name;
        }
      } else {
        const auto it = ros2_message_handler_map_.find(key);
        if (it != ros2_message_handler_map_.end()) {
          qos = it->second.ros_sub_handle_ptr->get_actual_qos().get_rmw_qos_profile();
          wire_topic = it->second.ros_sub_handle_ptr->get_topic_name();
        }
      }
    } else {
      const auto name = std::string(topic) + "/" + Ros2NameEncode(msg_type);
      if (publisher) {
        const auto it = publisher_map_.find(name);
        if (it != publisher_map_.end()) {
          qos = it->second->get_actual_qos().get_rmw_qos_profile();
          wire_topic = it->second->get_topic_name();
        }
      } else {
        const auto it = message_handler_map_.find(name);
        if (it != message_handler_map_.end()) {
          qos = it->second.ros_sub_handle_ptr->get_actual_qos().get_rmw_qos_profile();
          wire_topic = it->second.ros_sub_handle_ptr->get_topic_name();
        }
      }
    }
    if (!qos) return {};
    auto result = detail::DescribeQos(*qos);
    result.policies["qos_provenance"] = "actual_entity";
    if (!wire_topic.empty()) result.policies["wire_topic_name"] = wire_topic;
    size_t domain = 0;
    if (rcl_node_get_domain_id(ros2_node_ptr_->get_node_base_interface()->get_rcl_node_handle(), &domain) == RCL_RET_OK) {
      result.policies["domain_id"] = std::to_string(domain);
    }
    return result;
  } catch (const std::exception& error) {
    MW_ERROR("Cannot inspect ROS 2 endpoint QoS: {}", error.what());
    return {};
  }
}

runtime::core::transport::SerializedSubscriberHandle
Ros2TransportDriver::AttachSerializedSubscriber(
    const runtime::core::transport::SerializedSubscriberOptionsV2& options,
    runtime::core::transport::SerializedMessageCallback callback) noexcept {
  try {
    std::lock_guard lock(native_mutex_);
    MW_ASSERT(state_ == State::kInit || state_ == State::kStart, "ROS2 driver is not running.");
    MW_ASSERT(callback && !options.wire_topic_name.empty() && !options.wire_type_name.empty() &&
                  options.topic.msg_type == "ros2:" + options.wire_type_name &&
                  options.topic.serialization_type == "ros2",
              "Invalid native ROS2 subscription.");
    Options::QosOptions settings;
    for (const auto& configured : options_.sub_topics_options) {
      if (std::regex_match(options.topic.topic, std::regex(configured.topic_name))) {
        settings = configured.qos;
        break;
      }
    }
    ApplyQosDescriptor(options.requested_qos, settings);
    auto qos = GetQos(settings);
    ApplyQosProfileDescriptor(options.requested_qos, qos);
    auto state = std::make_shared<runtime::core::transport::SerializedCallback>(std::move(callback));
    auto subscription = ros2_node_ptr_->create_generic_subscription(
        options.wire_topic_name, options.wire_type_name, qos,
        [state, topic = options.topic](std::shared_ptr<rclcpp::SerializedMessage> data) {
          try {
            const auto& bytes = data->get_rcl_serialized_message();
            auto payload = std::shared_ptr<ibmw::util::BufferArrayView>(
                new ibmw::util::BufferArrayView(bytes.buffer, bytes.buffer_length),
                [data](auto* view) { delete view; });
            state->Invoke({.topic = topic,
                           .receive_timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count()),
                           .source_driver = "ros2", .payload = std::move(payload)});
          } catch (const std::exception& error) {
            MW_ERROR("Native ROS2 receive failed: {}", error.what());
          }
        });
    const uint64_t handle = next_native_handle_++;
    native_subscribers_.emplace(handle, NativeSubscriber{std::move(state), std::move(subscription)});
    return {handle};
  } catch (const std::exception& error) {
    MW_ERROR("Cannot attach native ROS2 subscriber: {}", error.what());
    return {};
  }
}

bool Ros2TransportDriver::DetachSerializedSubscriber(
    runtime::core::transport::SerializedSubscriberHandle handle) noexcept {
  NativeSubscriber subscriber;
  {
    std::lock_guard lock(native_mutex_);
    const auto found = native_subscribers_.find(handle.value);
    if (found == native_subscribers_.end()) return false;
    subscriber = std::move(found->second);
    native_subscribers_.erase(found);
  }
  subscriber.callback->Stop();
  subscriber.subscription.reset();
  return true;
}

bool Ros2TransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = send_type_wrapper.info;

    rclcpp::QoS qos(GetQos(Options::QosOptions()));

    auto find_qos_option = std::find_if(
        options_.pub_topics_options.begin(), options_.pub_topics_options.end(),
        [&info](const Options::PubTopicOptions& pub_option) {
          try {
            return std::regex_match(info.topic_name.begin(), info.topic_name.end(),
                                    std::regex(pub_option.topic_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       pub_option.topic_name, info.topic_name, e.what());
            return false;
          }
        });

    bool use_serialized = false;
    if (find_qos_option != options_.pub_topics_options.end()) {
      qos = GetQos(find_qos_option->qos);
      use_serialized = find_qos_option->use_serialized;
    }

    if (CheckRosMsg(info.msg_type)) {
      Key key{
          .topic_name = info.topic_name,
          .msg_type = info.msg_type};

      if (ros2_send_type_wrapper_map_.find(key) != ros2_send_type_wrapper_map_.end()) {
        return true;
      }

      auto unique_ptr = std::make_unique<rcl_publisher_t>(rcl_get_zero_initialized_publisher());

      rcl_publisher_t* publisher_ptr = unique_ptr.get();
      auto type_support = LoadRos2TypeSupport(info.msg_type);

      std::string ros2_topic_name = rclcpp::extend_name_with_sub_namespace(
          info.topic_name,
          ros2_node_ptr_->get_sub_namespace());

      rcl_publisher_options_t publisher_options = rcl_publisher_get_default_options();
      publisher_options.qos = qos.get_rmw_qos_profile();

      rcl_ret_t ret = rcl_publisher_init(
          publisher_ptr,
          ros2_node_ptr_->get_node_base_interface()->get_shared_rcl_node_handle().get(),
          type_support.handle,
          ros2_topic_name.c_str(),
          &publisher_options);

      if (ret != RMW_RET_OK) {
        MW_WARN("Ros2 publisher init failed, type '{}', error info: {}",
                   info.msg_type, rcl_get_error_string().str);
        rcl_reset_error();
        return false;
      }

      ros2_send_type_wrapper_map_.emplace(
          std::move(key),
          RosPubWrapper{
              .publisher_ptr = std::move(unique_ptr),
              .use_serialized = use_serialized,
              .type_support_library = std::move(type_support.library)});

      MW_INFO("ros2 driver register publish type for topic '{}' success.", info.topic_name);

      return true;
    }

    std::string real_ros2_topic_name = info.topic_name + "/" + Ros2NameEncode(info.msg_type);

    if (publisher_map_.find(real_ros2_topic_name) == publisher_map_.end()) {
      publisher_map_.emplace(
          real_ros2_topic_name,
          ros2_node_ptr_->create_publisher<ros2_ext_proto::msg::RosMsgWrapper>(real_ros2_topic_name, qos));
    }

    MW_INFO("ros2 driver register publish type for topic '{}' success, real ros2 topic name is '{}'.",
               info.topic_name, real_ros2_topic_name);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool Ros2TransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = message_handler.info;

    Options::QosOptions requested_qos;
    bool use_serialized = false;

    auto find_option = std::find_if(
        options_.sub_topics_options.begin(), options_.sub_topics_options.end(),
        [&info](const Options::SubTopicOptions& sub_option) {
          try {
            return std::regex_match(info.topic_name.begin(), info.topic_name.end(),
                                    std::regex(sub_option.topic_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       sub_option.topic_name, info.topic_name, e.what());
            return false;
          }
        });

    if (find_option != options_.sub_topics_options.end()) {
      requested_qos = find_option->qos;
      use_serialized = find_option->use_serialized;
    }
    ApplyQosDescriptor(message_handler.requested_qos, requested_qos);
    rclcpp::QoS qos(GetQos(requested_qos));
    ApplyQosProfileDescriptor(message_handler.requested_qos, qos);

    if (CheckRosMsg(info.msg_type)) {
      Key key{
          .topic_name = info.topic_name,
          .msg_type = info.msg_type};

      auto find_itr = ros2_message_handler_map_.find(key);
      if (find_itr != ros2_message_handler_map_.end()) {
        find_itr->second.msg_handler_tool_ptr->AddMessageHandlerWrapper(&message_handler);
        return true;
      }

      auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
      msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

      auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();
      auto type_support = LoadRos2TypeSupport(info.msg_type);
      const auto* ros2_type_support = type_support.handle;

      std::string ros2_topic_name = rclcpp::extend_name_with_sub_namespace(
          info.topic_name,
          ros2_node_ptr_->get_sub_namespace());

      auto node_topics_interface =
          rclcpp::node_interfaces::get_node_topics_interface(*ros2_node_ptr_);

#if RCLCPP_VERSION_MAJOR == 16
      rclcpp::SubscriptionFactory factory{
          [&message_handler, msg_handler_tool_ptr, use_serialized,
           ros2_type_support](
              rclcpp::node_interfaces::NodeBaseInterface* node_base,
              const std::string& topic_name,
              const rclcpp::QoS& qos) -> rclcpp::SubscriptionBase::SharedPtr {
            const rclcpp::SubscriptionOptionsWithAllocator<
                std::allocator<void>>& options =
                rclcpp::SubscriptionOptionsWithAllocator<std::allocator<void>>();

            std::shared_ptr<Ros2AdapterSubscription> subscriber =
                std::make_shared<Ros2AdapterSubscription>(
                    node_base,
                    *ros2_type_support,
                    topic_name,
                    options.to_rcl_subscription_options<void>(qos),
                    message_handler,
                    *msg_handler_tool_ptr,
                    use_serialized);
            return std::dynamic_pointer_cast<rclcpp::SubscriptionBase>(subscriber);
          }};
#elif RCLCPP_VERSION_MAJOR == 28
      rclcpp::SubscriptionFactory factory{
          [&message_handler, msg_handler_tool_ptr, use_serialized,
           ros2_type_support](
              rclcpp::node_interfaces::NodeBaseInterface* node_base,
              const std::string& topic_name,
              const rclcpp::QoS& qos) -> rclcpp::SubscriptionBase::SharedPtr {
            const rclcpp::SubscriptionOptionsWithAllocator<
                std::allocator<void>>& options =
                rclcpp::SubscriptionOptionsWithAllocator<std::allocator<void>>();

            std::shared_ptr<Ros2AdapterSubscription> subscriber =
                std::make_shared<Ros2AdapterSubscription>(
                    node_base,
                    *ros2_type_support,
                    topic_name,
                    options.to_rcl_subscription_options(qos),
                    message_handler,
                    *msg_handler_tool_ptr,
                    use_serialized);
            return std::dynamic_pointer_cast<rclcpp::SubscriptionBase>(subscriber);
          }};
#endif

      auto subscriber = node_topics_interface->create_subscription(ros2_topic_name, factory, qos);
      node_topics_interface->add_subscription(subscriber, nullptr);

      ros2_message_handler_map_.emplace(
          key,
          RosSubWrapper{
              .msg_handler_tool_ptr = std::move(msg_handler_tool_unique_ptr),
              .ros_sub_handle_ptr = std::move(subscriber),
              .type_support_library = std::move(type_support.library)});

      MW_INFO("subscribe topic '{}' success.", info.topic_name);

      return true;
    }

    std::string real_ros2_topic_name = info.topic_name + "/" + Ros2NameEncode(info.msg_type);

    auto find_itr = message_handler_map_.find(real_ros2_topic_name);
    if (find_itr != message_handler_map_.end()) {
      find_itr->second.msg_handler_tool_ptr->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    auto ros_sub_handle_ptr = ros2_node_ptr_->create_subscription<ros2_ext_proto::msg::RosMsgWrapper>(
        real_ros2_topic_name,
        qos,
        [this, topic_name = info.topic_name, msg_handler_tool_ptr](
            ros2_ext_proto::msg::RosMsgWrapper::UniquePtr wrapper_msg) {
          try {
            auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

            const std::string& serialization_type = wrapper_msg->serialization_type;
            ctx_ptr->SetSerializationType(serialization_type);

            size_t context_size = wrapper_msg->context.size() / 2;
            for (size_t ii = 0; ii < context_size; ++ii) {
              const auto& key = wrapper_msg->context[ii * 2];
              const auto& val = wrapper_msg->context[ii * 2 + 1];
              ctx_ptr->SetMetaValue(key, val);
            }

            ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

            if (state_.load() != State::kStart) [[unlikely]] {
              return;
            }

            msg_handler_tool_ptr->DoMessageCallback(
                ctx_ptr, serialization_type, static_cast<const void*>(wrapper_msg->data.data()), wrapper_msg->data.size());

          } catch (const std::exception& e) {
            MW_ERROR("{}", e.what());
          }
        });

    message_handler_map_.emplace(
        real_ros2_topic_name,
        SubWrapper{
            .msg_handler_tool_ptr = std::move(msg_handler_tool_unique_ptr),
            .ros_sub_handle_ptr = std::move(ros_sub_handle_ptr)});

    MW_INFO("subscribe topic '{}' success, real ros2 topic name is '{}'.",
               info.topic_name, real_ros2_topic_name);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void Ros2TransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    const auto& info = msg_wrapper.info;

    if (CheckRosMsg(info.msg_type)) {
      Key key{
          .topic_name = info.topic_name,
          .msg_type = info.msg_type};

      auto finditr = ros2_send_type_wrapper_map_.find(key);
      MW_REQUIRE(
          finditr != ros2_send_type_wrapper_map_.end(),
          "Publish msg type '{}' unregistered in ros2 transport driver, topic '{}', module '{}', lib path '{}'",
          info.msg_type, info.topic_name, info.module_name, info.pkg_path);

      rcl_publisher_t& publisher = *(finditr->second.publisher_ptr);
      bool use_serialized = finditr->second.use_serialized;

      if (use_serialized ||
          (msg_wrapper.msg_ptr == nullptr &&
           !msg_wrapper.serialization_cache.empty())) [[unlikely]] {
        std::string_view serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
        if (serialization_type.empty()) {
          serialization_type = info.msg_type_support_ref.DefaultSerializationType();
        }

        auto buffer_array_view_ptr = ibmw::runtime::core::transport::SerializeMsgWithCache(msg_wrapper, serialization_type);
        MW_REQUIRE(
            buffer_array_view_ptr,
            "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}",
            serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type);

        const auto* buffer_array_data = buffer_array_view_ptr->Data();
        const size_t buffer_array_len = buffer_array_view_ptr->Size();

        rcl_serialized_message_t rcl_ser = rmw_get_zero_initialized_serialized_message();
        rcl_ser.buffer = static_cast<uint8_t*>(const_cast<void*>(buffer_array_data[buffer_array_len - 1].data));
        rcl_ser.buffer_length = buffer_array_data[buffer_array_len - 1].len;
        rcl_ser.buffer_capacity = buffer_array_data[buffer_array_len - 1].len;

        rcl_ret_t ret = rcl_publish_serialized_message(&publisher, &rcl_ser, nullptr);
        if (ret != RMW_RET_OK) {
          MW_WARN(
              "Publish serialized msg type '{}' failed in ros2 transport driver, topic '{}', module '{}', lib path '{}', error info: {}",
              info.msg_type, info.topic_name, info.module_name, info.pkg_path,
              rcl_get_error_string().str);
          rcl_reset_error();
        }
        return;
      }

      ibmw::runtime::core::transport::CheckMsg(msg_wrapper);
      rcl_ret_t ret = rcl_publish(&publisher, msg_wrapper.msg_ptr, nullptr);
      if (ret != RMW_RET_OK) {
        MW_WARN(
            "Publish msg type '{}' failed in ros2 transport driver, topic '{}', module '{}', lib path '{}', error info: {}",
            info.msg_type, info.topic_name, info.module_name, info.pkg_path,
            rcl_get_error_string().str);
        rcl_reset_error();
      }
      return;
    }

    std::string real_ros2_topic_name = info.topic_name + "/" + Ros2NameEncode(info.msg_type);

    auto finditr = publisher_map_.find(real_ros2_topic_name);
    MW_REQUIRE(
        finditr != publisher_map_.end(),
        "Publish msg type '{}' unregistered in ros2 transport driver, topic '{}', module '{}', lib path '{}'",
        info.msg_type, info.topic_name, info.module_name, info.pkg_path);

    auto ros2_publisher_ptr = finditr->second;

    auto publish_type_support_ref = info.msg_type_support_ref;

    std::string_view serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    auto buffer_array_view_ptr = ibmw::runtime::core::transport::SerializeMsgWithCache(msg_wrapper, serialization_type);
    MW_REQUIRE(
        buffer_array_view_ptr,
        "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}",
        serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type);

    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t msg_size = buffer_array_view_ptr->BufferSize();

    ros2_ext_proto::msg::RosMsgWrapper wrapper_msg;
    wrapper_msg.serialization_type = serialization_type;

    auto [meta_key_vals_array, meta_key_vals_array_len] = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    wrapper_msg.context.reserve(meta_key_vals_array_len);
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      wrapper_msg.context.emplace_back(ibmw::util::ToStdStringView(meta_key_vals_array[ii]));
    }

    wrapper_msg.data.resize(msg_size);

    auto* cur_pos = wrapper_msg.data.data();
    for (size_t ii = 0; ii < buffer_array_len; ++ii) {
      memcpy(cur_pos, buffer_array_data[ii].data, buffer_array_data[ii].len);
      cur_pos += buffer_array_data[ii].len;
    }

    ros2_publisher_ptr->publish(wrapper_msg);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

rclcpp::QoS Ros2TransportDriver::GetQos(const Options::QosOptions& qos_option) {
  rclcpp::QoS qos(qos_option.depth);

  if (qos_option.history == "keep_last") {
    qos.keep_last(qos_option.depth);
  } else if (qos_option.history == "keep_all") {
    qos.history(rclcpp::HistoryPolicy::KeepAll);
  } else {
    qos.history(rclcpp::HistoryPolicy::SystemDefault);
  }

  if (qos_option.reliability == "reliable") {
    qos.reliability(rclcpp::ReliabilityPolicy::Reliable);
  } else if (qos_option.reliability == "best_effort") {
    qos.reliability(rclcpp::ReliabilityPolicy::BestEffort);
  } else {
    qos.reliability(rclcpp::ReliabilityPolicy::SystemDefault);
  }

  if (qos_option.durability == "volatile") {
    qos.durability(rclcpp::DurabilityPolicy::Volatile);
  } else if (qos_option.durability == "transient_local") {
    qos.durability(rclcpp::DurabilityPolicy::TransientLocal);
  } else {
    qos.durability(rclcpp::DurabilityPolicy::SystemDefault);
  }

  if (qos_option.liveliness == "automatic") {
    qos.liveliness(rclcpp::LivelinessPolicy::Automatic);
  } else if (qos_option.liveliness == "manual_by_topic") {
    qos.liveliness(rclcpp::LivelinessPolicy::ManualByTopic);
  } else {
    qos.liveliness(rclcpp::LivelinessPolicy::SystemDefault);
  }

  if (qos_option.deadline != -1) {
    qos.deadline(rclcpp::Duration::from_nanoseconds(qos_option.deadline * 1000000));
  }

  if (qos_option.lifespan != -1) {
    qos.lifespan(rclcpp::Duration::from_nanoseconds(qos_option.lifespan * 1000000));
  }

  if (qos_option.liveliness_lease_duration != -1) {
    qos.liveliness_lease_duration(rclcpp::Duration::from_nanoseconds(qos_option.liveliness_lease_duration * 1000000));
  }
  return qos;
}

}  // namespace ibmw::extensions::ros2_extension
