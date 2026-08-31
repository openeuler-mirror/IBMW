// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "record_playback_ext/capture/capture_source.h"
#include "engine/runtime_graph/topic_advertisement.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

class RuntimeTapClient {
 public:
  RuntimeTapClient() = default;
  virtual ~RuntimeTapClient() = default;

  virtual void StartTap(
      const TopicAdvertisement& advertisement,
      ibmw::transport::SerializedMessageCallback callback) = 0;
  virtual void StopTap() = 0;
};

class RuntimeTapSource final : public CaptureSource {
 public:
  RuntimeTapSource(
      TopicAdvertisement advertisement,
      std::unique_ptr<RuntimeTapClient> client);
  ~RuntimeTapSource() override;

  void Start(
      ibmw::transport::SerializedMessageCallback callback)
      override;
  void Stop() override;
  std::string Description() const override;

 private:
  TopicAdvertisement advertisement_;
  std::unique_ptr<RuntimeTapClient> client_;
  bool started_ = false;
};

}  // namespace ibmw::extensions::record_playback_extension
