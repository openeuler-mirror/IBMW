// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/parallel_executor.h"
#include "engine/thread_tools.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::ParallelExecutor::Options> {
  using Options = ibmw::runtime::core::executor::ParallelExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["thread_num"] = rhs.thread_num;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["queue_threshold"] = rhs.queue_threshold;
    node["threshold_alarm_interval_ms"] = rhs.threshold_alarm_interval_ms;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["thread_num"]) rhs.thread_num = node["thread_num"].as<uint32_t>();
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

thread_local const ParallelExecutor* ParallelExecutor::tl_current_executor_ = nullptr;

void ParallelExecutor::Initialize(std::string_view name, YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "ParallelExecutor can only be initialized once.");

  name_ = std::string(name);
  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  queue_threshold_ = options_.queue_threshold;
  queue_warn_threshold_ = queue_threshold_ * 0.95;

  MW_REQUIRE(
      options_.thread_num > 0,
      "Invalide tbb pool executor options, thread num is zero.");

  for (uint32_t ii = 0; ii < options_.thread_num; ++ii) {
    threads_.emplace_back([this, ii] {
      ++work_thread_num_;

      tl_current_executor_ = this;

      std::string threadname = name_;
      if (options_.thread_num > 1)
        threadname = threadname + "." + std::to_string(ii);

      try {
        util::SetNameForCurrentThread(threadname);
        util::BindCpuForCurrentThread(options_.thread_bind_cpu);
        util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
      } catch (const std::exception& e) {
        MW_WARN("Set thread policy for tbb pool executor '{}' get exception, {}",
                   Name(), e.what());
      }

      ibmw::executor::Task task;
      while (true) {
#if defined(IBMW_SELFDEV_CONCURRENT)
        try {
          while (qu_.TryPop(task)) {
            task();
            --queue_task_num_;
          }
        } catch (const std::exception& e) {
          MW_FATAL("Tbb pool executor '{}' run loop get exception, {}",
                      Name(), e.what());
        }

        if (state_.load() == State::kShutdown) break;

        try {
          // Pop() returns false once Shutdown() aborted the queue.
          if (qu_.Pop(task)) {
            task();
            --queue_task_num_;
          }
        } catch (const std::exception& e) {
          MW_FATAL("Tbb pool executor '{}' run loop get exception, {}",
                      Name(), e.what());
        }
#else
        try {
          while (qu_.try_pop(task)) {
            task();
            --queue_task_num_;
          }
        } catch (const std::exception& e) {
          MW_FATAL("Tbb pool executor '{}' run loop get exception, {}",
                      Name(), e.what());
        }

        if (state_.load() == State::kShutdown) break;

        try {
          qu_.pop(task);
          task();
          --queue_task_num_;
        } catch (const tbb::user_abort& e) {
        } catch (const std::exception& e) {
          MW_FATAL("Tbb pool executor '{}' run loop get exception, {}",
                      Name(), e.what());
        }
#endif
      }

      tl_current_executor_ = nullptr;

      --work_thread_num_;
    });
  }

  options_node = options_;
}

void ParallelExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void ParallelExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  while (work_thread_num_.load()) {
#if defined(IBMW_SELFDEV_CONCURRENT)
    qu_.Abort();  // sticky: one call is enough, repeating is harmless
#else
    qu_.abort();
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  for (auto itr = threads_.begin(); itr != threads_.end();) {
    if (itr->joinable()) itr->join();
    threads_.erase(itr++);
  }
}

bool ParallelExecutor::IsInCurrentExecutor() const noexcept {
  return tl_current_executor_ == this;
}

void ParallelExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  auto cur_state = state_.load(std::memory_order_acquire);
  if (cur_state != State::kInit && cur_state != State::kStart) [[unlikely]] {
    MW_ERROR("Tbb pool executor '{}' can only execute task when state is 'Init' or 'Start'.",
                name_);

    return;
  }

  // Approximate pre-check: use load() for threshold test to avoid inflating
  // the counter before the task is actually inserted.
  uint32_t cur_queue_task_num = queue_task_num_.load(std::memory_order_relaxed);

  if (options_.threshold_alarm_interval_ms >= 0 && cur_queue_task_num >= queue_threshold_) [[unlikely]] {
    MW_WARN_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                        "The number of tasks in the tbb pool executor '{}' has reached the threshold '{}', the task will not be delivered.",
                        name_, queue_threshold_);
    return;
  }

  if (options_.threshold_alarm_interval_ms >= 0 && cur_queue_task_num >= queue_warn_threshold_) [[unlikely]] {
    MW_WARN_INTERVAL(static_cast<uint64_t>(options_.threshold_alarm_interval_ms),
                        "The number of tasks in the tbb pool executor '{}' is about to reach the threshold: '{} / {}'.",
                        name_, cur_queue_task_num, queue_threshold_);
  }

  // Count before inserting: a worker may pop and decrement the task before
  // this thread runs another instruction, which would otherwise wrap the
  // unsigned counter and make the threshold check reject tasks.
  ++queue_task_num_;
  try {
#if defined(IBMW_SELFDEV_CONCURRENT)
    if (!qu_.Emplace(std::move(task))) [[unlikely]] {
      --queue_task_num_;
      MW_ERROR("Tbb pool executor '{}' can only execute task when state is 'Init' or 'Start'.",
                  name_);
      return;
    }
#else
    qu_.emplace(std::move(task));
#endif
  } catch (const std::exception& e) {
    --queue_task_num_;
    MW_ERROR("Tbb pool executor '{}' execute task get exception, {}", name_, e.what());
  }
}

std::chrono::system_clock::time_point ParallelExecutor::Now() const noexcept {
  MW_ERROR("Tbb pool executor '{}' does not support timer schedule.", Name());
  return std::chrono::system_clock::time_point();
}

void ParallelExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  MW_ERROR("Tbb pool executor '{}' does not support timer schedule.", Name());
}

}  // namespace ibmw::runtime::core::executor
