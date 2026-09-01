// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/topic_io/serialized_message_source.h"
#include "engine/runtime_graph/topic_advertisement.h"
#include "engine/runtime_graph/topic_graph.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicAdvertisement;
using ibmw::runtime::core::runtime_graph::TopicGraph;

TopicGraph BuildCaptureGraph(
    std::vector<ibmw::runtime::core::runtime_graph::RuntimeRegistration> runtimes);
using ibmw::runtime::core::topic_io::SerializedMessageSource;

struct CapturePlanEntry {
  TopicAdvertisement advertisement;
  std::string source_name;
  std::string capture_mode;
  std::string description;
  std::unique_ptr<SerializedMessageSource> source;
};

class CapturePlan {
 public:
  CapturePlan() = default;
  ~CapturePlan();

  CapturePlan(CapturePlan&&) noexcept = default;
  CapturePlan& operator=(CapturePlan&&) noexcept = default;
  CapturePlan(const CapturePlan&) = delete;
  CapturePlan& operator=(const CapturePlan&) = delete;

  void Add(CapturePlanEntry entry);
  void StartAll(
      ibmw::transport::SerializedMessageCallback callback);
  void StopAll();

  const std::vector<CapturePlanEntry>& Entries() const noexcept {
    return entries_;
  }

 private:
  std::vector<CapturePlanEntry> entries_;
  size_t started_count_ = 0;
};

class BagCapturePlanner {
 public:
  using TransportSourceFactory = std::function<std::unique_ptr<SerializedMessageSource>(
      const TopicAdvertisement&, std::string_view driver_name)>;
  using RuntimeTapSourceFactory = std::function<std::unique_ptr<SerializedMessageSource>(
      const TopicAdvertisement&)>;

  explicit BagCapturePlanner(const TopicGraph& graph) : graph_(graph) {}

  void RegisterTransportSourceFactory(
      std::string driver_name,
      TransportSourceFactory factory);
  void SetRuntimeTapSourceFactory(RuntimeTapSourceFactory factory);

  CapturePlan BuildPlan(const std::vector<std::string>& topics) const;

 private:
  struct Candidate {
    int priority;
    std::string source_name;
    std::string capture_mode;
    TopicAdvertisement advertisement;
    std::function<std::unique_ptr<SerializedMessageSource>()> create;
  };

  std::vector<Candidate> CandidatesFor(
      std::string_view topic_name) const;

  const TopicGraph& graph_;
  std::unordered_map<std::string, TransportSourceFactory>
      transport_factories_;
  RuntimeTapSourceFactory runtime_tap_factory_;
};

}  // namespace ibmw::extensions::record_playback_extension
