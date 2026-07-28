// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/capture/runtime_tap_source.h"

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

RuntimeTapSource::RuntimeTapSource(
    TopicAdvertisement advertisement,
    std::unique_ptr<RuntimeTapClient> client)
    : advertisement_(std::move(advertisement)), client_(std::move(client)) {
  MW_ASSERT(client_ != nullptr, "RuntimeTapClient cannot be null.");
}

RuntimeTapSource::~RuntimeTapSource() {
  try {
    Stop();
  } catch (...) {
  }
}

void RuntimeTapSource::Start(
    ibmw::runtime::core::transport::SerializedMessageCallback callback) {
  MW_ASSERT(!started_, "Runtime tap is already started.");
  client_->StartTap(advertisement_, std::move(callback));
  started_ = true;
}

void RuntimeTapSource::Stop() {
  if (!started_) {
    return;
  }
  client_->StopTap();
  started_ = false;
}

std::string RuntimeTapSource::Description() const {
  return "runtime tap (runtime " + advertisement_.runtime_id + ")";
}

}  // namespace ibmw::extensions::record_playback_extension
