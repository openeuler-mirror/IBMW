// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/discovery/bag_topic_graph.h"

#include <string>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

TopicAdvertisement MakeAdvertisement(
    std::string topic,
    std::string publisher_id,
    bool published = true,
    bool hidden = false) {
  TopicAdvertisement advertisement;
  advertisement.publisher_id = std::move(publisher_id);
  advertisement.topic.topic = std::move(topic);
  advertisement.topic.msg_type = "example/Message";
  advertisement.topic.serialization_type = "cdr";
  advertisement.published = published;
  advertisement.hidden = hidden;
  return advertisement;
}

RuntimeRegistration MakeRuntime(
    std::string runtime_id,
    std::vector<TopicAdvertisement> topics) {
  RuntimeRegistration runtime;
  runtime.runtime_id = std::move(runtime_id);
  runtime.control_socket = "/tmp/" + runtime.runtime_id + ".sock";
  runtime.topics = std::move(topics);
  return runtime;
}

TEST(BagTopicGraphTest, MergeDeduplicatesAndSortsAdvertisements) {
  BagTopicGraph graph;
  graph.MergeRuntime(MakeRuntime(
      "runtime-b",
      {MakeAdvertisement("/z", "publisher-2"),
       MakeAdvertisement("/a", "publisher-1")}));
  graph.MergeRuntime(MakeRuntime(
      "runtime-a", {MakeAdvertisement("/z", "publisher-1")}));

  EXPECT_EQ(graph.Topics(), (std::vector<std::string>{"/a", "/z"}));
  auto advertisements = graph.Find("/z");
  ASSERT_EQ(advertisements.size(), 2);
  EXPECT_EQ(advertisements[0].runtime_id, "runtime-a");
  EXPECT_EQ(advertisements[1].runtime_id, "runtime-b");
  EXPECT_EQ(advertisements[0].control_socket, "/tmp/runtime-a.sock");

  graph.MergeRuntime(MakeRuntime(
      "runtime-a",
      {MakeAdvertisement("/z", "publisher-1", false)}));
  advertisements = graph.Find("/z");
  ASSERT_EQ(advertisements.size(), 2);
  EXPECT_FALSE(advertisements[0].published);
}

TEST(BagTopicGraphTest, ReplaceRuntimesRemovesStaleTopics) {
  BagTopicGraph graph;
  graph.MergeRuntime(MakeRuntime(
      "old-runtime", {MakeAdvertisement("/old", "publisher")}));

  graph.ReplaceRuntimes({MakeRuntime(
      "new-runtime", {MakeAdvertisement("/new", "publisher")})});

  EXPECT_TRUE(graph.Find("/old").empty());
  EXPECT_EQ(graph.Topics(), (std::vector<std::string>{"/new"}));
  ASSERT_EQ(graph.Find("/new").size(), 1);
  EXPECT_EQ(graph.Find("/new")[0].runtime_id, "new-runtime");
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
