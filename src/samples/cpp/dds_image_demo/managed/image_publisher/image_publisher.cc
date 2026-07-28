// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "image_publisher/image_publisher.h"

#include <chrono>
#include <iostream>

#include "type_support/dds/dds_transport.h"
#include "protocol/image_msg.h"

namespace ibmw::examples::dds_image_demo {

bool ImagePublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;
  transport_ = core.GetTransport();

  // Get publisher
  auto publisher = transport_.GetPublisher("image_topic");
  if (!publisher) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
                  "Failed to get publisher for topic: image_topic");
    return false;
  }

  // Register native DDS message type (single-template-param API)
  bool ret = ibmw::transport::RegisterSendType<ImageMsg>(publisher);
  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
                  "Failed to register DDS publish type for ImageMsg");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
               "ImagePublisher module initialized. "
               "Type: ibmw::examples::dds_image_demo::ImageMsg, "
               "buffer size: {} bytes",
               kImageBufferSize);

  return true;
}

bool ImagePublisherModule::Run() {
  running_ = true;
  publish_thread_ = std::thread(&ImagePublisherModule::PublishLoop, this);
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "ImagePublisher module started.");
  return true;
}

void ImagePublisherModule::Stop() {
  if (running_) {
    running_ = false;
    if (publish_thread_.joinable()) {
      publish_thread_.join();
    }
  }
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo, "ImagePublisher module shutdown.");
}

void ImagePublisherModule::GenerateTestImage(uint32_t width, uint32_t height,
                                              uint64_t frame_id,
                                              uint8_t* buffer,
                                              size_t& data_size) {
  // Generate a simple test pattern (RGB8)
  // Pattern: gradient based on frame_id
  data_size = width * height * 3;  // RGB8: 3 bytes per pixel

  uint8_t r_base = (frame_id * 10) % 256;
  uint8_t g_base = (frame_id * 20) % 256;
  uint8_t b_base = (frame_id * 30) % 256;

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      size_t idx = (y * width + x) * 3;
      buffer[idx + 0] = (r_base + x * 255 / width) % 256;
      buffer[idx + 1] = (g_base + y * 255 / height) % 256;
      buffer[idx + 2] = (b_base + (x + y) * 255 / (width + height)) % 256;
    }
  }
}

void ImagePublisherModule::PublishLoop() {
  auto publisher = transport_.GetPublisher("image_topic");

  uint64_t frame_count = 0;

  // Test with VGA resolution
  const uint32_t test_width = 640;
  const uint32_t test_height = 480;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
               "Publishing test images: {}x{} RGB8, rate=10Hz, "
               "expected data size={} bytes",
               test_width, test_height, test_width * test_height * 3);

  while (running_) {
    // Create native DDS message
    ImageMsg msg;

    // Set metadata
    msg.frame_id = frame_count;
    msg.width = test_width;
    msg.height = test_height;
    msg.encoding = ImageEncoding::RGB8;

    // Get current timestamp (microseconds since epoch)
    auto now = std::chrono::system_clock::now();
    msg.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch())
                        .count();

    // Generate test image data
    size_t data_size = 0;
    GenerateTestImage(test_width, test_height, frame_count,
                      msg.data.data(), data_size);
    msg.data_size = static_cast<uint32_t>(data_size);

    // Publish using native DDS transport (single-template-param API)
    ibmw::transport::Send<ImageMsg>(publisher, msg);

    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                 "Published frame #{}, size={} bytes, timestamp={} us",
                 frame_count, data_size, msg.timestamp);

    frame_count++;

    // Publish at 10 Hz
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace ibmw::examples::dds_image_demo
