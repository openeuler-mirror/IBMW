// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "cpp/util/schema_support.h"
#include "ibmw_msgs/msg/joint_command_array.hpp"
#include "ibmw_msgs/msg/message_header.hpp"
#include "type_support/ros2/ros2_schema_support.h"

namespace {

TEST(Ros2SchemaSupport, PreservesMsgNamespaceForNestedCrossPackageArrays) {
  const auto load_definition = [](std::string_view type) {
    if (type == "ibmw_msgs/msg/JointCommandArray") {
      return std::string(
          "MessageHeader header\nJointCommand[] joints\n");
    }
    if (type == "ibmw_msgs/msg/MessageHeader") {
      return std::string(
          "builtin_interfaces/Time stamp\nstring frame_id\nuint32 sequence\n");
    }
    if (type == "ibmw_msgs/msg/JointCommand") {
      return std::string("string name\nfloat64 position\n");
    }
    if (type == "builtin_interfaces/msg/Time") {
      return std::string("int32 sec\nuint32 nanosec\n");
    }
    return std::string();
  };
  const ibmw::util::SchemaSupportRef support(
      ibmw::ros2::GetRos2MessageSchemaSupport<
          ibmw_msgs::msg::JointCommandArray>(load_definition));
  ASSERT_TRUE(support);
  EXPECT_EQ(support.SchemaName(), "ibmw_msgs/msg/JointCommandArray");
  EXPECT_EQ(support.SchemaEncoding(), "ros2msg");

  const std::string_view schema = support.SchemaData();
  EXPECT_NE(schema.find("ibmw_msgs/msg/MessageHeader header\n"),
            std::string_view::npos);
  EXPECT_NE(schema.find("ibmw_msgs/msg/JointCommand[] joints\n"),
            std::string_view::npos);
  EXPECT_NE(schema.find("MSG: ibmw_msgs/msg/MessageHeader\n"),
            std::string_view::npos);
  EXPECT_NE(schema.find("builtin_interfaces/msg/Time stamp\n"),
            std::string_view::npos);
  EXPECT_NE(schema.find("MSG: builtin_interfaces/msg/Time\n"),
            std::string_view::npos);
  EXPECT_EQ(
      support.SchemaHash(), ibmw::util::ComputeSchemaHash(support.SchemaData()));
}

TEST(Ros2SchemaSupport, CanonicalDefinitionBuilderUsesStableNestedNames) {
  const auto descriptor = ibmw::ros2::BuildRos2SchemaDescriptor(
      "demo/msg/Root", [](std::string_view type) {
        if (type == "demo/msg/Root") {
          return std::string(
              "Child child\n"
              "other_pkg/External[2] fixed\n"
              "other_pkg/msg/External[] dynamic\n"
              "other_pkg/External[<=4] bounded\n"
              "string<=8 label \"ready\"\n"
              "uint32 LIMIT=7\n");
        }
        if (type == "demo/msg/Child") {
          return std::string("int32 value 5\n");
        }
        if (type == "other_pkg/msg/External") {
          return std::string("wstring<=16 text\n");
        }
        return std::string();
      });
  const std::string expected =
            "demo/msg/Child child\n"
            "other_pkg/msg/External[2] fixed\n"
            "other_pkg/msg/External[] dynamic\n"
            "other_pkg/msg/External[<=4] bounded\n"
            "string<=8 label \"ready\"\n"
            "uint32 LIMIT=7\n"
            "================================================================================\n"
            "MSG: demo/msg/Child\n"
            "int32 value 5\n"
            "================================================================================\n"
            "MSG: other_pkg/msg/External\n"
            "wstring<=16 text\n";
  EXPECT_EQ(descriptor.schema_data, expected);
  EXPECT_EQ(descriptor.schema_hash, ibmw::util::ComputeSchemaHash(expected));
  EXPECT_EQ(descriptor.schema_name, "demo/msg/Root");
  EXPECT_EQ(descriptor.schema_encoding, "ros2msg");
}

}  // namespace
