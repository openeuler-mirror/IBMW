// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ibmw::extensions::record_playback_extension {

class CompressionTaskPool {
 public:
  explicit CompressionTaskPool(size_t thread_count);
  ~CompressionTaskPool();

  CompressionTaskPool(const CompressionTaskPool&) = delete;
  CompressionTaskPool& operator=(const CompressionTaskPool&) = delete;

  std::future<std::string> Submit(std::function<std::string()> task);
  void Shutdown();

 private:
  void Run();

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace ibmw::extensions::record_playback_extension
