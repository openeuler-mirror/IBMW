// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "image_subscriber/image_subscriber.h"

#include <chrono>
#include <iostream>

#include "internal/typesupport/dds/dds_transport.h"
#include "protocol/image_msg.h"

namespace ibmw::examples::dds_image_demo {

bool ImageSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;
  transport_ = core.GetTransport();

  // Get subscriber
  auto subscriber = transport_.GetSubscriber("image_topic");
  if (!subscriber) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
                  "Failed to get subscriber for topic: image_topic");
    return false;
  }

  // Subscribe to native DDS messages (single-template-param API)
  auto logger = core_.GetLogger();

  bool ret = ibmw::transport::OnMessage<ImageMsg>(
      subscriber,
      [logger](const std::shared_ptr<const ImageMsg>& msg) {
        // Process image in callback
        auto now = std::chrono::system_clock::now();
        auto recv_timestamp =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count();

        // Calculate latency
        int64_t latency_us = recv_timestamp - msg->timestamp;

        // Verify image data (check first few pixels)
        const uint8_t* data = msg->data.data();
        uint32_t pixel_count = msg->width * msg->height;

        // Calculate simple checksum for verification
        uint64_t checksum = 0;
        for (uint32_t i = 0; i < std::min(100u, pixel_count * 3); ++i) {
          checksum += data[i];
        }

        MW_LOG(logger, ibmw::common::util::kLogLevelInfo,
                     "Received frame #{}: {}x{}, encoding={}, "
                     "data_size={} bytes, latency={} us, "
                     "checksum(first 100 bytes)={}, "
                     "first pixel RGB=({},{},{})",
                     msg->frame_id, msg->width, msg->height,
                     static_cast<uint32_t>(msg->encoding),
                     msg->data_size, latency_us, checksum,
                     static_cast<int>(data[0]),
                     static_cast<int>(data[1]),
                     static_cast<int>(data[2]));
      });

  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
                  "Failed to subscribe to DDS ImageMsg messages");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
               "ImageSubscriber module initialized. "
               "Type: ibmw::examples::dds_image_demo::ImageMsg");
  return true;
}

bool ImageSubscriberModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "ImageSubscriber module started.");
  return true;
}

void ImageSubscriberModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "ImageSubscriber module shutdown.");
}

}  // namespace ibmw::examples::dds_image_demo
