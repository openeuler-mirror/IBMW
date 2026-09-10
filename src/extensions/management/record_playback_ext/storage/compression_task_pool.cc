// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/storage/compression_task_pool.h"

#include <memory>
#include <utility>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

CompressionTaskPool::CompressionTaskPool(size_t thread_count) {
  MW_ASSERT(thread_count > 0, "Compression worker count cannot be zero.");
  workers_.reserve(thread_count);
  for (size_t index = 0; index < thread_count; ++index) {
    workers_.emplace_back([this] { Run(); });
  }
}

CompressionTaskPool::~CompressionTaskPool() {
  Shutdown();
}

std::future<std::string> CompressionTaskPool::Submit(
    std::function<std::string()> task) {
  MW_ASSERT(task, "Compression task cannot be empty.");
  auto packaged = std::make_shared<std::packaged_task<std::string()>>(
      std::move(task));
  auto result = packaged->get_future();
  {
    std::lock_guard lock(mutex_);
    MW_ASSERT(!stopping_, "Compression task pool is stopping.");
    tasks_.emplace_back([packaged] { (*packaged)(); });
  }
  ready_.notify_one();
  return result;
}

void CompressionTaskPool::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

void CompressionTaskPool::Run() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    task();
  }
}

}  // namespace ibmw::extensions::record_playback_extension
