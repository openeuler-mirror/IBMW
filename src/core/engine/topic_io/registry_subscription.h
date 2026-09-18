// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/runtime_graph/topic_advertisement.h"
#include "engine/topic_io/serialized_message_source.h"
#include "engine/transport/serialized_subscriber.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::topic_io {

struct RegistrySubscription {
  runtime_graph::TopicAdvertisement publisher;
  std::string driver;
};

using RegistrySubscriptionFilter = std::function<bool(const RegistrySubscription&)>;

// Registry discovery does not prescribe a capture transport. Pick one direct
// route covering all publishers, or one advertised Tap for each publisher.
std::vector<RegistrySubscription> SelectRegistrySubscriptions(
    const std::vector<runtime_graph::TopicAdvertisement>& advertisements,
    const RegistrySubscriptionFilter& available = {});
std::string RegistrySubscriptionKey(const RegistrySubscription& subscription);
std::string RegistrySubscriptionConnectionKey(const RegistrySubscription& subscription);
YAML::Node RegistrySubscriptionConfig(const RegistrySubscription& subscription);
transport::SerializedSubscriberOptionsV2 RegistrySubscriberOptions(
    const RegistrySubscription& subscription);

// Shared by record and active topic commands. Sources must be stopped and
// destroyed before this owner. Typed subscriptions attach before StartPending;
// after startup only previously registered typed subscriptions can be reused.
class RegistrySubscriptionRuntime final {
 public:
  RegistrySubscriptionRuntime();
  ~RegistrySubscriptionRuntime();
  bool Available(const RegistrySubscription& subscription) const;
  std::unique_ptr<SerializedMessageSource> Create(const RegistrySubscription& subscription);
  void StartPending();
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ibmw::runtime::core::topic_io
