// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "record_playback_ext/capture/capture_source.h"
#include "record_playback_ext/discovery/bag_topic_graph.h"

namespace ibmw::extensions::record_playback_extension {

struct CapturePlanEntry {
  TopicAdvertisement advertisement;
  std::string source_name;
  std::string description;
  std::unique_ptr<CaptureSource> source;
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
  using TransportSourceFactory = std::function<std::unique_ptr<CaptureSource>(
      const TopicAdvertisement&, std::string_view driver_name)>;
  using RuntimeTapSourceFactory = std::function<std::unique_ptr<CaptureSource>(
      const TopicAdvertisement&)>;

  explicit BagCapturePlanner(const BagTopicGraph& graph) : graph_(graph) {}

  void RegisterTransportSourceFactory(
      std::string driver_name,
      TransportSourceFactory factory);
  void SetRuntimeTapSourceFactory(RuntimeTapSourceFactory factory);

  CapturePlan BuildPlan(const std::vector<std::string>& topics) const;

 private:
  struct Candidate {
    int priority;
    std::string source_name;
    TopicAdvertisement advertisement;
    std::function<std::unique_ptr<CaptureSource>()> create;
  };

  std::vector<Candidate> CandidatesFor(
      std::string_view topic_name) const;

  const BagTopicGraph& graph_;
  std::unordered_map<std::string, TransportSourceFactory>
      transport_factories_;
  RuntimeTapSourceFactory runtime_tap_factory_;
};

}  // namespace ibmw::extensions::record_playback_extension
