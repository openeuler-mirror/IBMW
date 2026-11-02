// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-033: JSON input cannot make resizable arrays claim unbounded storage.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <std_msgs/msg/float64_multi_array.hpp>

#include "type_support/ros2/ros2_text_serialization.h"

namespace ibmw::ros2::text_detail {
namespace {

using Msg = std_msgs::msg::Float64MultiArray;

Json DataOf(size_t count) {
  auto data = Json::array();
  for (size_t i = 0; i < count; ++i) data.push_back(static_cast<double>(i));
  return Json{{"data", data}};
}

TEST(Ros2TextSerializationTest, ReadsWithinTheArrayBudget) {
  Msg msg;
  ReadMessage(DataOf(3), &msg, Members<Msg>());
  EXPECT_EQ(msg.data, (std::vector<double>{0, 1, 2}));
  EXPECT_EQ(WriteMessage(&msg, Members<Msg>())["data"].size(), 3u);
}

TEST(Ros2TextSerializationTest, ArraysBeyondTheBudgetAreRejected) {
  Msg msg;
  size_t budget = 10 * sizeof(double);
  ReadMessage(DataOf(10), &msg, Members<Msg>(), 0, budget);
  EXPECT_EQ(budget, 0u);

  budget = 10 * sizeof(double);
  EXPECT_THROW(ReadMessage(DataOf(11), &msg, Members<Msg>(), 0, budget), std::length_error);
}

TEST(Ros2TextSerializationTest, NestedArraysShareTheBudget) {
  const size_t dim_size = sizeof(std_msgs::msg::MultiArrayDimension);
  Json input = DataOf(2);
  input["layout"] = Json{{"dim", Json::array({Json::object(), Json::object()})}};

  Msg msg;
  size_t budget = 2 * dim_size + 2 * sizeof(double);
  ReadMessage(input, &msg, Members<Msg>(), 0, budget);
  EXPECT_EQ(msg.layout.dim.size(), 2u);
  EXPECT_EQ(budget, 0u);

  budget = 2 * dim_size + 2 * sizeof(double) - 1;
  EXPECT_THROW(ReadMessage(input, &msg, Members<Msg>(), 0, budget), std::length_error);
}

TEST(Ros2TextSerializationTest, DefaultBudgetRejectsAmplifiedNestedArrays) {
  // `{}` costs 3 bytes of JSON but a whole MultiArrayDimension once decoded.
  const size_t count = kMaxTextArrayBytes / sizeof(std_msgs::msg::MultiArrayDimension) + 1;
  Json dims = Json::array();
  dims.get_ref<Json::array_t&>().resize(count, Json::object());
  Msg msg;
  EXPECT_THROW(ReadMessage(Json{{"layout", Json{{"dim", dims}}}}, &msg, Members<Msg>()),
               std::length_error);
  EXPECT_TRUE(msg.layout.dim.empty());
}

TEST(Ros2TextSerializationTest, UnknownFieldsAreRejected) {
  Msg msg;
  EXPECT_THROW(ReadMessage(Json{{"nope", 1}}, &msg, Members<Msg>()), std::invalid_argument);
}

}  // namespace
}  // namespace ibmw::ros2::text_detail
