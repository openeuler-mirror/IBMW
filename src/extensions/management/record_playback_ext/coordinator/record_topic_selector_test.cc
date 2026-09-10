// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/record_topic_selector.h"

#include <string>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

TopicAdvertisement MakeAdvertisement(
    std::string topic,
    bool published = true,
    bool hidden = false) {
  TopicAdvertisement advertisement;
  advertisement.publisher_id = topic;
  advertisement.topic.topic = std::move(topic);
  advertisement.topic.msg_type = "example/Message";
  advertisement.topic.serialization_type = "cdr";
  advertisement.published = published;
  advertisement.hidden = hidden;
  return advertisement;
}

BagTopicGraph MakeGraph() {
  RuntimeRegistration runtime;
  runtime.runtime_id = "runtime";
  runtime.topics = {
      MakeAdvertisement("/camera/raw"),
      MakeAdvertisement("/camera/debug"),
      MakeAdvertisement("/hidden", true, true),
      MakeAdvertisement("/leaf", false)};
  BagTopicGraph graph;
  graph.MergeRuntime(runtime);
  return graph;
}

TEST(RecordTopicSelectorTest, RegexAndExcludeFiltersAreApplied) {
  RecordTopicSelector selector({
      .regex = "^/camera/",
      .exclude_regex = "debug$"});

  EXPECT_EQ(selector.Select(MakeGraph()),
            (std::set<std::string>{"/camera/raw"}));
}

TEST(RecordTopicSelectorTest, AllHonorsHiddenAndUnpublishedOptions) {
  RecordTopicSelector default_selector({.all = true});
  EXPECT_EQ(
      default_selector.Select(MakeGraph()),
      (std::set<std::string>{"/camera/debug", "/camera/raw"}));

  RecordTopicSelector inclusive_selector({
      .all = true,
      .include_hidden_topics = true,
      .include_unpublished_topics = true});
  EXPECT_EQ(
      inclusive_selector.Select(MakeGraph()),
      (std::set<std::string>{
          "/camera/debug", "/camera/raw", "/hidden", "/leaf"}));
}

TEST(RecordTopicSelectorTest, ExplicitTopicsAreAuthoritativeExceptForExcludes) {
  RecordTopicSelector selector({
      .explicit_topics = {"/missing", "/hidden"},
      .exclude_regex = "hidden$"});

  EXPECT_EQ(selector.Select(MakeGraph()),
            (std::set<std::string>{"/missing"}));
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
