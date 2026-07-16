// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "subscriber_module/subscriber_module.h"
#include "compressed_image_dds_types.h"

using ibmw::protocols::sensor::CompressedImage;

namespace iceoryx_inproc_demo {

bool SubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup");

  auto subscriber = core_.GetTransport().GetSubscriber("compressed_image_topic");

  bool ret = ibmw::transport::OnMessage<CompressedImage>(
      subscriber,
      [this](const std::shared_ptr<const CompressedImage>& msg) {
        const size_t size = msg->data().size();

        // Re-derive the expected pattern from the first byte to verify
        // end-to-end payload integrity over the iceoryx shared-memory hop.
        uint8_t first_byte = size > 0 ? msg->data()[0] : 0;
        bool integrity_ok = true;
        for (size_t i = 0; i < size; ++i) {
          if (msg->data()[i] != static_cast<uint8_t>(first_byte + (i & 0xFF) - (0 & 0xFF))) {
            integrity_ok = false;
            break;
          }
        }

        MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
               "Received compressed_image format='{}' frame_id='{}' "
               "seq={} payload_size={} bytes integrity={}",
               msg->format(), msg->header().frame_id(),
               msg->header().sequence_num(), size,
               integrity_ok ? "OK" : "MISMATCH");
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Subscribe failed!");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Setup succeeded.");
  return true;
}

bool SubscriberModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Run");
  return true;
}

void SubscriberModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "SubscriberModule Stop");
}

}  // namespace iceoryx_inproc_demo
