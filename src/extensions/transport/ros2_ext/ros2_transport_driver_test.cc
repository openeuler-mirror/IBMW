// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_ext/ros2_transport_driver.h"

#include <future>
#include <thread>
#include <unistd.h>

#include "gtest/gtest.h"

namespace ibmw::extensions::ros2_extension {
namespace {
using namespace std::chrono_literals;

TEST(Ros2SerializedCaptureTest, DynamicSubscriptionRetainsPayloadAndDrainsBeforeDetach) {
  auto context = std::make_shared<rclcpp::Context>();
  rclcpp::InitOptions init;
  init.set_domain_id(83);
  init.shutdown_on_signal = false;
  context->init(0, nullptr, init);
  auto node = std::make_shared<rclcpp::Node>(
      "ibmw_capture_test", rclcpp::NodeOptions().context(context));
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  Ros2TransportDriver driver;
  driver.SetNodePtr(node);
  driver.Initialize(YAML::Node(YAML::NodeType::Map));
  driver.Start();
  const std::string topic = "/ibmw_capture_" + std::to_string(::getpid());
  auto publisher = node->create_generic_publisher(topic, "std_msgs/msg/String", rclcpp::QoS(10));
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto released = release.get_future().share();
  std::atomic<unsigned> count = 0;
  std::shared_ptr<ibmw::util::BufferArrayView> retained;
  const auto handle = driver.AttachSerializedSubscriber({
      .topic = {.topic = topic, .msg_type = "ros2:std_msgs/msg/String", .serialization_type = "ros2"},
      .requested_qos = {.policies = {{"reliability", "reliable"}, {"durability", "volatile"}}},
      .wire_topic_name = topic, .wire_type_name = "std_msgs/msg/String"}, [&](auto&& message) {
    if (++count == 1) {
      retained = message.payload;
      EXPECT_EQ(message.topic.serialization_type, "ros2");
      EXPECT_FALSE(message.identity.IsValid());
      entered.set_value();
      released.wait();
    }
  });
  ASSERT_TRUE(handle);
  std::jthread spinner([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      executor.spin_some();
      std::this_thread::sleep_for(2ms);
    }
  });
  // CDR-LE std_msgs/String("hello"), including the string terminator.
  const uint8_t bytes[] = {0, 1, 0, 0, 6, 0, 0, 0, 'h', 'e', 'l', 'l', 'o', 0};
  rclcpp::SerializedMessage message(sizeof(bytes));
  auto& buffer = message.get_rcl_serialized_message();
  std::memcpy(buffer.buffer, bytes, sizeof(bytes));
  buffer.buffer_length = sizeof(bytes);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (ready.wait_for(30ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline) {
    publisher->publish(message);
  }
  if (ready.wait_for(0ms) != std::future_status::ready) {
    release.set_value();
    FAIL() << "Native ROS2 subscription received no samples";
  }
  auto detached = std::async(std::launch::async, [&] { return driver.DetachSerializedSubscriber(handle); });
  EXPECT_EQ(detached.wait_for(50ms), std::future_status::timeout);
  release.set_value();
  EXPECT_TRUE(detached.get());
  const auto stopped_count = count.load();
  publisher->publish(message);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(count.load(), stopped_count);
  spinner.request_stop();
  spinner.join();
  driver.Shutdown();
  context->shutdown("test complete");
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->JoinToString(), std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes)));
}

}  // namespace
}  // namespace ibmw::extensions::ros2_extension
