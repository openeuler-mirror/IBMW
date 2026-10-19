// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "engine/discovery/discovery_provider.h"
#include "engine/runtime_graph/topic_advertisement.h"
#include "engine/transport/serialized_subscriber.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::topic_io {

// Only the selected transport and its executors are copied into the observer.
// Discovery's domain is authoritative; relative connection paths are resolved
// against the optional configuration file. No business modules are loaded.
YAML::Node SubscriptionRuntimeConfig(
    ibmw::discovery::DiscoveryOptions& source,
    const std::filesystem::path& config_file);

class TopicSubscription final {
 public:
  struct Options {
    ibmw::discovery::DiscoveryOptions source;
    std::string topic;
    // Uses the type name printed by topic type for the selected source.
    std::string type;
    std::filesystem::path config_file;
    ibmw::transport::QosDescriptor qos;
    // Runs on the lifecycle thread before attaching a registry subscription.
    // Consumers can retain local type metadata without exposing it to network discovery.
    std::function<void(const runtime_graph::TopicAdvertisement&)> prepare_registry;
  };

  explicit TopicSubscription(Options options);
  ~TopicSubscription();
  TopicSubscription(const TopicSubscription&) = delete;
  TopicSubscription& operator=(const TopicSubscription&) = delete;

  void Start(ibmw::transport::SerializedMessageCallback callback);
  // Refresh registry capture routes / network wire subscriptions without creating
  // one network subscription per publisher. Call from the lifecycle thread.
  bool Refresh();
  uint64_t DroppedMessages() const;
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ibmw::runtime::core::topic_io
