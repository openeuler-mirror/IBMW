// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/log_util.h"
#include "engine/topic_io/serialized_message_source.h"
#include "engine/transport/serialized_transport_driver.h"

namespace ibmw::runtime::core::topic_io {

class TransportSubscriptionSource final : public SerializedMessageSource {
 public:
  TransportSubscriptionSource(
      std::string driver_name,
      ibmw::runtime::core::transport::SerializedTransportOperationsV2 operations,
      ibmw::runtime::core::transport::SerializedSubscriberOptionsV2 options,
      const ibmw::common::util::LoggerWrapper& logger);
  ~TransportSubscriptionSource() override;

  void Start(
      ibmw::transport::SerializedMessageCallback callback)
      override;
  void Stop() override;
  std::string Description() const override;

 private:
  std::string driver_name_;
  ibmw::runtime::core::transport::SerializedTransportOperationsV2 operations_;
  ibmw::runtime::core::transport::SerializedSubscriberOptionsV2 options_;
  const ibmw::common::util::LoggerWrapper& logger_;
  ibmw::runtime::core::transport::SerializedSubscriberHandle handle_;
};

}  // namespace ibmw::runtime::core::topic_io
