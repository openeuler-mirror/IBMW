// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/recorder/record_queue.h"

#include <utility>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

RecordQueue::RecordQueue(Options options) : options_(options) {
  MW_ASSERT(
      options_.max_messages > 0 || options_.max_bytes > 0,
      "RecordQueue must have a message or byte limit.");
}

RecordQueue::PushResult RecordQueue::Push(
    ibmw::runtime::core::transport::SerializedMessage message) {
  if (!message.payload) {
    std::lock_guard lock(mutex_);
    ++dropped_;
    return PushResult::kInvalid;
  }

  const size_t payload_size = message.PayloadSize();
  std::unique_lock lock(mutex_);

  if (closed_) {
    return PushResult::kClosed;
  }

  if (options_.max_bytes > 0 && payload_size > options_.max_bytes) {
    ++dropped_;
    return PushResult::kDropped;
  }

  if (options_.full_policy == FullPolicy::kDropNewest &&
      !FitsLocked(payload_size)) {
    ++dropped_;
    return PushResult::kDropped;
  }

  if (options_.full_policy == FullPolicy::kBlock) {
    not_full_cv_.wait(lock, [this, payload_size] {
      return closed_ || FitsLocked(payload_size);
    });
    if (closed_) {
      return PushResult::kClosed;
    }
  }

  queued_bytes_ += payload_size;
  queue_.emplace_back(std::move(message));
  ++pushed_;
  lock.unlock();
  not_empty_cv_.notify_one();
  return PushResult::kPushed;
}

std::optional<ibmw::runtime::core::transport::SerializedMessage>
RecordQueue::Pop() {
  std::unique_lock lock(mutex_);
  not_empty_cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
  return PopLocked();
}

std::optional<ibmw::runtime::core::transport::SerializedMessage>
RecordQueue::TryPop() {
  std::lock_guard lock(mutex_);
  return PopLocked();
}

void RecordQueue::Close() {
  std::lock_guard lock(mutex_);
  if (closed_) {
    return;
  }
  closed_ = true;
  not_empty_cv_.notify_all();
  not_full_cv_.notify_all();
}

RecordQueue::Statistics RecordQueue::GetStatistics() const {
  std::lock_guard lock(mutex_);
  return Statistics{
      .pushed = pushed_,
      .popped = popped_,
      .dropped = dropped_,
      .queued_messages = queue_.size(),
      .queued_bytes = queued_bytes_,
      .closed = closed_};
}

bool RecordQueue::FitsLocked(size_t payload_size) const noexcept {
  if (options_.max_messages > 0 && queue_.size() >= options_.max_messages) {
    return false;
  }
  if (options_.max_bytes > 0 &&
      payload_size > options_.max_bytes - queued_bytes_) {
    return false;
  }
  return true;
}

std::optional<ibmw::runtime::core::transport::SerializedMessage>
RecordQueue::PopLocked() {
  if (queue_.empty()) {
    return std::nullopt;
  }

  auto message = std::move(queue_.front());
  queue_.pop_front();
  queued_bytes_ -= message.PayloadSize();
  ++popped_;
  not_full_cv_.notify_one();
  return message;
}

}  // namespace ibmw::extensions::record_playback_extension
