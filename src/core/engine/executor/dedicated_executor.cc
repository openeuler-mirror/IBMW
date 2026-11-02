// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/dedicated_executor.h"
#include "engine/thread_tools.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::DedicatedExecutor::Options> {
  using Options = ibmw::runtime::core::executor::DedicatedExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["queue_threshold"] = rhs.queue_threshold;
    node["threshold_alarm_interval_ms"] = rhs.threshold_alarm_interval_ms;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["thread_sched_policy"])
      rhs.thread_sched_policy = node["thread_sched_policy"].as<std::string>();

    if (node["thread_bind_cpu"])
      rhs.thread_bind_cpu = node["thread_bind_cpu"].as<std::vector<uint32_t>>();

    if (node["queue_threshold"])
      rhs.queue_threshold = node["queue_threshold"].as<uint32_t>();

    if (node["threshold_alarm_interval_ms"])
      rhs.threshold_alarm_interval_ms = node["threshold_alarm_interval_ms"].as<int32_t>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::executor {

void DedicatedExecutor::Initialize(std::string_view name, YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "DedicatedExecutor can only be initialized once.");

  name_ = std::string(name);
  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  queue_threshold_ = options_.queue_threshold;
  queue_warn_threshold_ = queue_threshold_ * 0.95;

  thread_ptr_ = std::make_unique<std::thread>([this]() {
    thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);

    try {
      util::EnsureAltSignalStackForCurrentThread();
      util::SetNameForCurrentThread(name_);
      util::BindCpuForCurrentThread(options_.thread_bind_cpu);
      util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
    } catch (const std::exception& e) {
      MW_WARN("Set thread policy for worker thread executor '{}' get exception, {}",
                 Name(), e.what());
    }

    while (state_.load() != State::kShutdown) {
      // Multi-producer-single-consumer optimization
      std::queue<ibmw::executor::Task> tmp_queue;

      {
        std::unique_lock<std::mutex> lck(mutex_);
        cond_.wait(lck, [this] { return !queue_.empty() || state_.load() == State::kShutdown; });
        queue_.swap(tmp_queue);
      }

      while (!tmp_queue.empty()) {
        auto& task = tmp_queue.front();

        try {
          task();
          --queue_task_num_;
        } catch (const std::exception& e) {
          MW_FATAL("Worker thread executor run task get exception, {}", e.what());
        }

        tmp_queue.pop();
      }
    }

    // Drain what was queued before shutdown. Dispatch re-checks the state under
    // mutex_, so once this swap observes an empty queue nothing else can enter.
    while (true) {
      std::queue<ibmw::executor::Task> tmp_queue;
      {
        std::unique_lock<std::mutex> lck(mutex_);
        if (queue_.empty()) break;
        queue_.swap(tmp_queue);
      }
      while (!tmp_queue.empty()) {
        tmp_queue.front()();
        --queue_task_num_;
        tmp_queue.pop();
      }
    }

    thread_id_.store(std::thread::id{}, std::memory_order_relaxed);
  });

  options_node = options_;
}

void DedicatedExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void DedicatedExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  {
    std::unique_lock<std::mutex> lck(mutex_);
    cond_.notify_one();
  }

  if (thread_ptr_ && thread_ptr_->joinable())
    thread_ptr_->join();

  thread_ptr_.reset();
}

void DedicatedExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  auto cur_state = state_.load(std::memory_order_acquire);
  if (cur_state != State::kInit && cur_state != State::kStart) [[unlikely]] {
    MW_ERROR("Worker thread executor '{}' can only execute task when state is 'Init' or 'Start'.",
                name_);
    return;
  }

  // Pre-check threshold using current count (approximate). The actual
  // increment happens after successful insertion to avoid counter divergence.
  uint32_t approx_count = queue_task_num_.load(std::memory_order_relaxed);

  if (options_.threshold_alarm_interval_ms >= 0 && approx_count >= queue_threshold_) [[unlikely]] {
    MW_WARN_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                        "The number of tasks in the worker thread executor '{}' has reached the threshold '{}', the task will not be delivered.",
                        name_, queue_threshold_);
    return;
  }

  if (options_.threshold_alarm_interval_ms >= 0 && approx_count >= queue_warn_threshold_) [[unlikely]] {
    MW_WARN_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                        "The number of tasks in the worker thread executor '{}' is about to reach the threshold: '{} / {}'.",
                        name_, approx_count, queue_threshold_);
  }

  {
    std::unique_lock<std::mutex> lck(mutex_);
    // Re-check under the lock: the worker drains the queue after shutdown.
    if (state_.load(std::memory_order_relaxed) == State::kShutdown) [[unlikely]] {
      MW_WARN("Worker thread executor '{}' is shutting down, the task is dropped.", name_);
      return;
    }
    queue_.emplace(std::move(task));
    ++queue_task_num_;
    cond_.notify_one();
  }
}

std::chrono::system_clock::time_point DedicatedExecutor::Now() const noexcept {
  MW_ERROR("Worker thread executor '{}' does not support timer schedule.", Name());
  return std::chrono::system_clock::time_point();
}

void DedicatedExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  MW_ERROR("Worker thread executor '{}' does not support timer schedule.", Name());
}

}  // namespace ibmw::runtime::core::executor
