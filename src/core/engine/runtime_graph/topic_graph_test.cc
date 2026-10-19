// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/topic_graph.h"

#include <string>

#include "gtest/gtest.h"

namespace ibmw::runtime::core::runtime_graph {
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

TEST(TopicGraphTest, MergeDeduplicatesAndSortsAdvertisements) {
  TopicGraph graph;
  graph.MergeRuntime(MakeRuntime(
      "runtime-b",
      {MakeAdvertisement("/shared", "publisher-b")}));
  graph.MergeRuntime(MakeRuntime(
      "runtime-a",
      {MakeAdvertisement("/shared", "publisher-a")}));

  const auto advertisements = graph.Find("/shared");
  ASSERT_EQ(advertisements.size(), 2u);
  EXPECT_EQ(advertisements[0].runtime_id, "runtime-a");
  EXPECT_EQ(advertisements[1].runtime_id, "runtime-b");
}

TEST(TopicGraphTest, ReplaceRuntimesRemovesStaleTopics) {
  TopicGraph graph;
  graph.ReplaceRuntimes({MakeRuntime(
      "old-runtime", {MakeAdvertisement("/old", "publisher")} )});
  graph.ReplaceRuntimes({MakeRuntime(
      "new-runtime", {MakeAdvertisement("/new", "publisher")} )});

  EXPECT_TRUE(graph.Find("/old").empty());
  ASSERT_EQ(graph.Find("/new").size(), 1u);
  EXPECT_EQ(graph.Find("/new")[0].runtime_id, "new-runtime");
}

TEST(TopicGraphTest, AllowsUnknownSchemaAlongsideDescribedEndpoint) {
  auto unknown = MakeRuntime(
      "runtime-unknown", {MakeAdvertisement("/shared", "publisher-unknown")});
  auto described = MakeRuntime(
      "runtime-described", {MakeAdvertisement("/shared", "publisher-described")});
  described.topics[0].topic.schema = {
      .type_name = "example/Message",
      .serialization_type = "cdr",
      .schema_name = "example",
      .schema_encoding = "text",
      .schema_data = "message Example {}",
      .schema_hash = "hash"};

  TopicGraph graph;
  EXPECT_NO_THROW(graph.ReplaceRuntimes({unknown, described}));
  EXPECT_EQ(graph.Find("/shared").size(), 2u);
}

TEST(TopicGraphTest, PreservesCaptureCapabilityFields) {
  auto advertisement = MakeAdvertisement("/topic", "publisher");
  advertisement.runtime_id = "runtime";
  advertisement.capture_capabilities["runtime-tap"] = {.mode = "runtime_tap"};
  advertisement.capture_capabilities["custom"] = {
      .abi_version = 1, .mode = "native_serialized", .priority = 17,
      .extension_name = "provider", .extension_path = "/provider.so",
      .provider_instance_option = "instance", .provider_clear_option = "clear",
      .provider_strip_leading_slash_option = "strip"};
  const auto restored =
      TopicAdvertisementFromJson(TopicAdvertisementToJson(advertisement));
  EXPECT_TRUE(restored.capture_capabilities.contains("runtime-tap"));
  EXPECT_EQ(restored.capture_capabilities.at("runtime-tap").mode, "runtime_tap");
  const auto& direct = restored.capture_capabilities.at("custom");
  EXPECT_EQ(direct.abi_version, 1u);
  EXPECT_EQ(direct.priority, 17u);
  EXPECT_EQ(direct.provider_instance_option, "instance");
  EXPECT_EQ(direct.provider_clear_option, "clear");
  EXPECT_EQ(direct.provider_strip_leading_slash_option, "strip");
}

}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
