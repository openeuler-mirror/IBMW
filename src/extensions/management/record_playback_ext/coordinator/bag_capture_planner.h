// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/topic_io/serialized_message_source.h"
#include "engine/topic_io/registry_subscription.h"
#include "engine/transport/serialized_subscriber.h"
#include "engine/runtime_graph/runtime_registration.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicAdvertisement;
using ibmw::runtime::core::topic_io::SerializedMessageSource;
using ibmw::runtime::core::topic_io::RegistrySubscriptionFilter;

// Network endpoints carry subscription addresses. Only registry endpoints have
// Runtime control information; no synthetic Runtime registrations are needed.
struct CaptureEndpoint {
  ibmw::transport::TopicDescriptor topic;
  std::string driver;
  std::string wire_topic_name;
  std::string wire_type_name;
  std::optional<TopicAdvertisement> registry_advertisement;
  bool published = true;
  bool hidden = false;

  bool SameSubscription(const CaptureEndpoint& other) const;
};

class CaptureGraph {
 public:
  void Add(CaptureEndpoint endpoint);
  const std::vector<CaptureEndpoint>& Find(std::string_view topic) const;
  std::vector<std::string> Topics() const;
  // Registry: choose a direct route, with per-publisher Tap as fallback.
  // Network: one reader for all publishers.
  std::vector<CaptureEndpoint> Subscriptions(
      std::string_view topic, const RegistrySubscriptionFilter& available = {}) const;

 private:
  std::map<std::string, std::vector<CaptureEndpoint>> topics_;
};

CaptureGraph BuildCaptureGraph(
    const std::vector<ibmw::runtime::core::runtime_graph::RuntimeRegistration>& runtimes);

struct CapturePlanEntry {
  CaptureEndpoint endpoint;
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
  void StartAll(ibmw::transport::SerializedMessageCallback callback);
  void StopAll();
  const std::vector<CapturePlanEntry>& Entries() const noexcept { return entries_; }

 private:
  std::vector<CapturePlanEntry> entries_;
  size_t started_count_ = 0;
};

class BagCapturePlanner {
 public:
  using SourceFactory = std::function<std::unique_ptr<SerializedMessageSource>(const CaptureEndpoint&)>;
  BagCapturePlanner(const CaptureGraph& graph, SourceFactory factory,
                    RegistrySubscriptionFilter available = {})
      : graph_(graph), factory_(std::move(factory)), available_(std::move(available)) {}
  CapturePlan BuildPlan(std::string_view topic) const;

 private:
  const CaptureGraph& graph_;
  SourceFactory factory_;
  RegistrySubscriptionFilter available_;
};

}  // namespace ibmw::extensions::record_playback_extension
