// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/util/type_support.h"
#include "engine/transport/transport_driver.h"
#include "record_playback_ext/capture/capture_source.h"
#include "engine/runtime_graph/topic_advertisement.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

class CachedTransportSubscriptionRegistry {
 public:
  struct SubscriptionState {
    std::mutex mutex;
    std::condition_variable idle_cv;
    bool active = false;
    size_t in_flight = 0;
    ibmw::transport::SerializedMessageCallback callback;
    TopicAdvertisement advertisement;
    std::string driver_name;
    std::exception_ptr error;
    std::atomic<uint64_t> loaned_sequence{0};
  };

  std::shared_ptr<SubscriptionState> Attach(
      std::string driver_name,
      ibmw::runtime::core::transport::TransportDriver& driver,
      const TopicAdvertisement& advertisement,
      ibmw::util::TypeSupportRef type_support,
      const ibmw::transport::QosDescriptor& requested_qos,
      ibmw::transport::SerializedMessageCallback callback);

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<SubscriptionState>> states_;
  std::vector<std::unique_ptr<
      ibmw::runtime::core::transport::MessageHandlerWrapper>> handlers_;
  std::vector<std::unique_ptr<
      ibmw::runtime::core::transport::LoanedMessageHandlerWrapper>>
      loaned_handlers_;
};

class CachedTransportSubscriptionSource final : public CaptureSource {
 public:
  CachedTransportSubscriptionSource(
      std::string driver_name,
      ibmw::runtime::core::transport::TransportDriver& driver,
      TopicAdvertisement advertisement,
      ibmw::util::TypeSupportRef type_support,
      CachedTransportSubscriptionRegistry& registry,
      ibmw::transport::QosDescriptor requested_qos = {});
  ~CachedTransportSubscriptionSource() override;

  void Start(
      ibmw::transport::SerializedMessageCallback callback)
      override;
  void Stop() override;
  std::string Description() const override;

 private:
  std::string driver_name_;
  ibmw::runtime::core::transport::TransportDriver& driver_;
  TopicAdvertisement advertisement_;
  ibmw::util::TypeSupportRef type_support_;
  CachedTransportSubscriptionRegistry& registry_;
  ibmw::transport::QosDescriptor requested_qos_;
  std::shared_ptr<CachedTransportSubscriptionRegistry::SubscriptionState>
      state_;
};

}  // namespace ibmw::extensions::record_playback_extension
