// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace ibmw::extensions::dds_extension {

/**
 * @brief Zero-copy performance monitor
 *
 * Periodically invokes a callback to output statistics.
 */
class ZeroCopyMonitor {
 public:
  using StatsPrinter = std::function<void()>;

  explicit ZeroCopyMonitor(StatsPrinter printer)
      : printer_(std::move(printer)), running_(false) {}

  ~ZeroCopyMonitor() {
    Stop();
  }

  void Start(uint32_t interval_sec) {
    if (running_.load()) return;

    interval_sec_ = interval_sec;
    running_.store(true);

    monitor_thread_ = std::thread([this]() {
      MonitorLoop();
    });
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_.exchange(false)) return;
    }

    cv_.notify_all();

    if (monitor_thread_.joinable()) {
      monitor_thread_.join();
    }
  }

  bool IsRunning() const { return running_.load(); }

 private:
  void MonitorLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_.load()) {
      if (cv_.wait_for(
              lock,
              std::chrono::seconds(interval_sec_),
              [this]() { return !running_.load(); })) {
        break;
      }
      lock.unlock();
      if (printer_) printer_();
      lock.lock();
    }
  }

 private:
  StatsPrinter printer_;
  std::atomic<bool> running_;
  uint32_t interval_sec_ = 60;
  std::thread monitor_thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace ibmw::extensions::dds_extension
