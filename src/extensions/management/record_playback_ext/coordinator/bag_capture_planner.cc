// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/bag_capture_planner.h"

#include <algorithm>
#include <exception>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

bool CaptureEndpoint::SameSubscription(const CaptureEndpoint& other) const {
  return driver == other.driver && wire_topic_name == other.wire_topic_name &&
         wire_type_name == other.wire_type_name &&
         topic.msg_type == other.topic.msg_type &&
         topic.serialization_type == other.topic.serialization_type &&
         topic.schema.schema_hash == other.topic.schema.schema_hash &&
         topic.qos.policies == other.topic.qos.policies &&
         registry_advertisement.has_value() == other.registry_advertisement.has_value() &&
         (!registry_advertisement || ibmw::runtime::core::topic_io::RegistrySubscriptionKey({*registry_advertisement, driver}) ==
                        ibmw::runtime::core::topic_io::RegistrySubscriptionKey({*other.registry_advertisement, other.driver}));
}

void CaptureGraph::Add(CaptureEndpoint endpoint) {
  topics_[endpoint.topic.topic].push_back(std::move(endpoint));
}

const std::vector<CaptureEndpoint>& CaptureGraph::Find(std::string_view topic) const {
  static const std::vector<CaptureEndpoint> empty;
  const auto found = topics_.find(std::string(topic));
  return found == topics_.end() ? empty : found->second;
}

std::vector<std::string> CaptureGraph::Topics() const {
  std::vector<std::string> result;
  for (const auto& [name, endpoints] : topics_) result.push_back(name);
  return result;
}

std::vector<CaptureEndpoint> CaptureGraph::Subscriptions(
    std::string_view topic, const RegistrySubscriptionFilter& available) const {
  std::vector<CaptureEndpoint> result;
  std::vector<TopicAdvertisement> registry_advertisements;
  for (const auto& endpoint : Find(topic)) {
    if (endpoint.registry_advertisement) registry_advertisements.push_back(*endpoint.registry_advertisement);
  }
  if (!registry_advertisements.empty()) {
    for (auto& selected : ibmw::runtime::core::topic_io::SelectRegistrySubscriptions(registry_advertisements, available)) {
      result.push_back({.topic = selected.publisher.topic, .driver = selected.driver,
                        .registry_advertisement = selected.publisher, .published = selected.publisher.published,
                        .hidden = selected.publisher.hidden});
    }
    return result;
  }
  for (const auto& endpoint : Find(topic)) {
    if (!endpoint.published) continue;
    if (!result.empty()) {
      const auto& selected = result.front().topic;
      MW_ASSERT(selected.msg_type == endpoint.topic.msg_type &&
                    selected.serialization_type == endpoint.topic.serialization_type &&
                    selected.schema.schema_hash == endpoint.topic.schema.schema_hash,
                "Topic '{}' has conflicting publisher types or schemas.", topic);
    }
    if (result.empty() || endpoint.registry_advertisement) {
      result.push_back(endpoint);
    } else {
      MW_ASSERT(result.front().SameSubscription(endpoint),
                "Topic '{}' has conflicting publisher types, schemas or subscription settings.", topic);
    }
  }
  return result;
}

CaptureGraph BuildCaptureGraph(
    const std::vector<ibmw::runtime::core::runtime_graph::RuntimeRegistration>& runtimes) {
  CaptureGraph graph;
  for (const auto& runtime : runtimes) {
    for (auto advertisement : runtime.topics) {
      if (advertisement.topic.schema.schema_hash.empty()) continue;
      advertisement.runtime_id = runtime.runtime_id;
      advertisement.control_socket = runtime.control_socket;
      graph.Add({.topic = advertisement.topic,
                 .registry_advertisement = advertisement,
                 .published = advertisement.published,
                 .hidden = advertisement.hidden});
    }
  }
  return graph;
}

CapturePlan::~CapturePlan() {
  try {
    StopAll();
  } catch (...) {
  }
}

void CapturePlan::Add(CapturePlanEntry entry) {
  MW_ASSERT(started_count_ == 0, "Cannot modify a started CapturePlan.");
  MW_ASSERT(entry.source != nullptr, "CapturePlan source cannot be null.");
  entries_.emplace_back(std::move(entry));
}

void CapturePlan::StartAll(
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(started_count_ == 0, "CapturePlan is already started.");
  try {
    for (auto& entry : entries_) {
      entry.source->Start(callback);
      ++started_count_;
    }
  } catch (...) {
    try {
      StopAll();
    } catch (...) {
    }
    throw;
  }
}

void CapturePlan::StopAll() {
  std::exception_ptr first_error;
  while (started_count_ > 0) {
    --started_count_;
    try {
      entries_[started_count_].source->Stop();
    } catch (...) {
      if (!first_error) {
        first_error = std::current_exception();
      }
    }
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

CapturePlan BagCapturePlanner::BuildPlan(std::string_view topic) const {
  CapturePlan plan;
  const auto endpoints = graph_.Subscriptions(topic, available_);
  MW_ASSERT(!endpoints.empty(), "No publisher is available for topic '{}'.", topic);
  for (const auto& endpoint : endpoints) {
    auto source = factory_(endpoint);
    MW_ASSERT(source, "Cannot create capture source for '{}'.", topic);
    const auto description = source->Description();
    plan.Add({.endpoint = endpoint, .description = description, .source = std::move(source)});
  }
  return plan;
}

}  // namespace ibmw::extensions::record_playback_extension
