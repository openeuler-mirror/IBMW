// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/bag_capture_planner.h"

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

TEST(BagCaptureGraphTest, SchemaLessEndpointsRemainVisibleOnlyInGeneralDiscovery) {
  ibmw::runtime::core::runtime_graph::RuntimeRegistration runtime{.runtime_id = "runtime"};
  TopicAdvertisement publisher{
      .runtime_id = "runtime",
      .publisher_id = "publisher",
      .topic = {.topic = "/topic", .msg_type = "message",
                .serialization_type = "json"},
      .published = true};
  publisher.topic.schema.schema_hash = "schema-hash";
  auto subscriber = publisher;
  subscriber.publisher_id = "subscriber";
  subscriber.published = false;
  subscriber.topic.schema = {};
  auto unknown = subscriber;
  unknown.topic.topic = "/unknown";
  unknown.published = true;
  runtime.topics = {publisher, subscriber, unknown};

  TopicGraph discovery;
  discovery.MergeRuntime(runtime);
  const auto capture = BuildCaptureGraph({runtime});
  EXPECT_EQ(discovery.Find("/topic").size(), 2u);
  EXPECT_EQ(discovery.Find("/unknown").size(), 1u);
  const auto endpoints = capture.Find("/topic");
  ASSERT_EQ(endpoints.size(), 1u);
  EXPECT_EQ(endpoints.front().publisher_id, "publisher");
  EXPECT_TRUE(capture.Find("/unknown").empty());
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
