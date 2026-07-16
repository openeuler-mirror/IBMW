// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/watchdog_executor.h"
#include "engine/thread_tools.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::WatchdogExecutor::Options> {
  using Options = ibmw::runtime::core::executor::WatchdogExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["name"] = rhs.name;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["queue_threshold"] = rhs.queue_threshold;
    node["threshold_alarm_interval_ms"] = rhs.threshold_alarm_interval_ms;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["name"])
      rhs.name = node["name"].as<std::string>();

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

void WatchdogExecutor::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "WatchdogExecutor can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  name_ = options_.name;

  queue_threshold_ = options_.queue_threshold;
  queue_warn_threshold_ = queue_threshold_ * 0.95;

  thread_ptr_ = std::make_unique<std::thread>([this]() {
    thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);

    try {
      util::SetNameForCurrentThread(name_);
      util::BindCpuForCurrentThread(options_.thread_bind_cpu);
      util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
    } catch (const std::exception& e) {
      MW_WARN("Set thread policy for watchdog executor '{}' get exception, {}",
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
          MW_FATAL("Watchdog executor run task get exception, {}", e.what());
        }

        tmp_queue.pop();
      }
    }

    // Run once more after shutdown, no need for locks since no more tasks will enter the queue
    while (!queue_.empty()) {
      auto& task = queue_.front();

      try {
        task();
        --queue_task_num_;
      } catch (const std::exception& e) {
        MW_FATAL("Watchdog executor run task get exception, {}", e.what());
      }

      queue_.pop();
    }

    thread_id_.store(std::thread::id{}, std::memory_order_relaxed);
  });

  options_node = options_;

  MW_INFO("[Watchdog] initialized");
}

void WatchdogExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void WatchdogExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  {
    std::unique_lock<std::mutex> lck(mutex_);
    cond_.notify_one();
  }

  if (thread_ptr_ && thread_ptr_->joinable())
    thread_ptr_->join();

  thread_ptr_.reset();

  MW_INFO("[Watchdog] stopped");
}
// todo
void WatchdogExecutor::Dispatch(ibmw::executor::Task&& task) {
  auto cur_state = state_.load(std::memory_order_acquire);
  if (cur_state != State::kInit && cur_state != State::kStart) [[unlikely]] {
    MW_LOG(simple_logger_, ibmw::common::util::kLogLevelError, "Watchdog executor can only execute task when state is 'Init' or 'Start'.");

    return;
  }

  // Pre-check threshold using current count (approximate). The actual
  // increment happens after successful insertion to avoid counter divergence.
  uint32_t approx_count = queue_task_num_.load(std::memory_order_relaxed);

  if (options_.threshold_alarm_interval_ms >= 0 && approx_count >= queue_threshold_) [[unlikely]] {
    MW_LOG_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                           simple_logger_, ibmw::common::util::kLogLevelWarn,
                           "The number of tasks in the watchdog executor has reached the threshold '{}', the task will not be delivered.",
                           queue_threshold_);
    return;
  }

  if (options_.threshold_alarm_interval_ms >= 0 && approx_count >= queue_warn_threshold_) [[unlikely]] {
    MW_LOG_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                           simple_logger_, ibmw::common::util::kLogLevelWarn,
                           "The number of tasks in the watchdog executor is about to reach the threshold: '{} / {}'",
                           approx_count, queue_threshold_);
  }

  {
    std::unique_lock<std::mutex> lck(mutex_);
    queue_.emplace(std::move(task));
    ++queue_task_num_;
    cond_.notify_one();
  }
}

}  // namespace ibmw::runtime::core::executor
