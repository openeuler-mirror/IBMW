// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "cpp/executor/executor.h"

namespace ibmw::runtime::core::logger {

// Runs `flush` on `log_executor` after the log tasks already queued there and
// waits for it until `deadline`. Used by LoggerDriverBase::FlushForCrash():
// the crash report is logged and then the process ends, while the console and
// rotate_file drivers write on their log executor, so without this wait the
// report is cut off or lost.
//
// Returns false without waiting when called on the log executor itself: the
// queued tasks cannot run before the caller returns, and flushing inline could
// touch a stream that the interrupted task is writing. Also returns false when
// the executor does not run the task before the deadline (stuck or stopped).
// The deadline bounds the wait for the task (checked every 1 ms), not the
// Dispatch() call, which takes the executor's queue lock.
template <typename Flush>
bool FlushOnLogExecutor(ibmw::executor::ExecutorHandle& log_executor, Flush&& flush,
                        std::chrono::steady_clock::time_point deadline) noexcept {
  try {
    if (!log_executor || log_executor.IsInCurrentExecutor()) return false;

    auto done = std::make_shared<std::atomic_bool>(false);
    log_executor.Dispatch([done, flush = std::forward<Flush>(flush)]() mutable {
      flush();
      done->store(true, std::memory_order_release);
    });

    while (!done->load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace ibmw::runtime::core::logger
