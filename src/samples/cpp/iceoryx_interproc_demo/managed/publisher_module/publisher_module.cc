// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"
#include "compressed_image_dds_types.h"

#include <array>
#include <chrono>
#include <thread>

using ibmw::protocols::sensor::CompressedImage;

namespace iceoryx_interproc_demo {

namespace {

// Walk through several Iceoryx LoanShm doublings when shm_init_size is
// set very small (256 bytes in publisher.yaml). Cycling small -> medium
// -> large -> medium keeps SHM growth visible across runs.
constexpr std::array<size_t, 4> kPayloadSizes = {
    1 * 1024,        // 1 KB
    16 * 1024,       // 16 KB
    256 * 1024,      // 256 KB
    1 * 1024 * 1024  // 1 MB
};

}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup (interproc, publisher process)");

  publisher_ = core_.GetTransport().GetPublisher("compressed_image_topic");

  bool ret = ibmw::transport::RegisterSendType<CompressedImage>(publisher_);
  if (!ret) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup succeeded.");
  return true;
}

bool PublisherModule::Run() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Run");
  running_ = true;
  std::thread([this]() { PublishLoop(); }).detach();
  return true;
}

void PublisherModule::Stop() {
  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Stop");
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void PublisherModule::PublishLoop() {
  while (running_) {
    const size_t payload_size = kPayloadSizes[msg_count_ % kPayloadSizes.size()];

    CompressedImage msg;
    msg.format("jpeg");
    msg.header().frame_id("camera_link");
    msg.header().sequence_num(msg_count_);
    msg.header().time_stamp(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()));

    auto& data = msg.data();
    data.resize(payload_size);
    const auto pattern = static_cast<uint8_t>(msg_count_ & 0xFF);
    for (size_t i = 0; i < payload_size; ++i) {
      data[i] = static_cast<uint8_t>(pattern + (i & 0xFF));
    }

    ibmw::transport::Send(publisher_, msg);

    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
           "[PUB] Published compressed_image #{} payload_size={} bytes pattern=0x{:02x}",
           msg_count_, payload_size, pattern);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

}  // namespace iceoryx_interproc_demo
