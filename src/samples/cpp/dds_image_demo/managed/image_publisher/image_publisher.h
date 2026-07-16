// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"

namespace ibmw::examples::dds_image_demo {

class ImagePublisherModule : public ibmw::Module {
 public:
  ImagePublisherModule() = default;
  ~ImagePublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ImagePublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  void GenerateTestImage(uint32_t width, uint32_t height, uint64_t frame_id,
                         uint8_t* buffer, size_t& data_size);

 private:
  ibmw::Runtime core_;
  ibmw::transport::TransportRef transport_;

  std::atomic<bool> running_{false};
  std::thread publish_thread_;
};

}  // namespace ibmw::examples::dds_image_demo
