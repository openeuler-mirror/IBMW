// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/thread_pool_executor.h"
#include "engine/thread_tools.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::ThreadPoolExecutor::Options> {
  using Options = ibmw::runtime::core::executor::ThreadPoolExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["thread_num"] = rhs.thread_num;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["timeout_alarm_threshold_us"] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            rhs.timeout_alarm_threshold_us)
            .count());
    node["use_system_clock"] = rhs.use_system_clock;
    node["timeout_alarm_interval_ms"] = rhs.timeout_alarm_interval_ms;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["thread_num"]) rhs.thread_num = node["thread_num"].as<uint32_t>();
    if (node["thread_sched_policy"])
      rhs.thread_sched_policy = node["thread_sched_policy"].as<std::string>();
    if (node["thread_bind_cpu"])
      rhs.thread_bind_cpu = node["thread_bind_cpu"].as<std::vector<uint32_t>>();
    if (node["timeout_alarm_threshold_us"])
      rhs.timeout_alarm_threshold_us = std::chrono::microseconds(
          node["timeout_alarm_threshold_us"].as<uint64_t>());
    if (node["use_system_clock"])
      rhs.use_system_clock = node["use_system_clock"].as<bool>();
    if (node["timeout_alarm_interval_ms"])
      rhs.timeout_alarm_interval_ms = node["timeout_alarm_interval_ms"].as<int32_t>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::executor {

thread_local const ThreadPoolExecutor* ThreadPoolExecutor::tl_current_executor_ = nullptr;

void ThreadPoolExecutor::Initialize(std::string_view name,
                                    YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "ThreadPoolExecutor can only be initialized once.");

  name_ = std::string(name);

  if (options_node && !options_node.IsNull()) {
    options_ = options_node.as<Options>();
    if (options_node["shutdown_grace_ms"])
      shutdown_grace_ms_ = options_node["shutdown_grace_ms"].as<int32_t>();
  }

  start_sys_tp_ = std::chrono::system_clock::now();
  start_std_tp_ = std::chrono::steady_clock::now();

  MW_REQUIRE(
      options_.thread_num > 0,
      "Invalide async pool executor options, thread num is zero.");

  io_ptr_ = std::make_unique<asio::io_context>(options_.thread_num);
  work_guard_ptr_ = std::make_unique<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      io_ptr_->get_executor());

  for (uint32_t ii = 0; ii < options_.thread_num; ++ii) {
    // Count only threads that actually start, so a failed std::thread
    // construction cannot leave Shutdown() waiting for a thread that never ran.
    {
      std::lock_guard<std::mutex> lck(exit_mutex_);
      ++running_threads_;
    }
    try {
      threads_.emplace_back([this, ii] {
        tl_current_executor_ = this;

        std::string threadname = name_;
        if (options_.thread_num > 1)
          threadname = threadname + "." + std::to_string(ii);

        try {
          util::SetNameForCurrentThread(threadname);
          util::BindCpuForCurrentThread(options_.thread_bind_cpu);
          util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
        } catch (const std::exception& e) {
          MW_WARN("Set thread policy for async pool executor '{}' get exception, {}",
                     Name(), e.what());
        }

        try {
          io_ptr_->run();
        } catch (const std::exception& e) {
          MW_FATAL("Async pool executor '{}' run loop get exception, {}",
                      Name(), e.what());
        }

        tl_current_executor_ = nullptr;

        std::lock_guard<std::mutex> lck(exit_mutex_);
        if (--running_threads_ == 0) exit_cv_.notify_all();
      });
    } catch (...) {
      std::lock_guard<std::mutex> lck(exit_mutex_);
      --running_threads_;
      throw;
    }
  }

  options_node = options_;
  options_node["shutdown_grace_ms"] = shutdown_grace_ms_;
}

void ThreadPoolExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void ThreadPoolExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (work_guard_ptr_) work_guard_ptr_->reset();

  // Queued handlers (and tasks they chain) get 'shutdown_grace_ms' to finish.
  // A pending DispatchAt() timer would otherwise keep run() alive until it
  // expires, so after the grace period the io_context is stopped, which
  // discards everything still queued (timers and ready tasks alike).
  // Called from one of our own workers, that worker counts as running, so
  // there is nothing to wait for (joining itself fails below as before).
  if (io_ptr_ && !IsInCurrentExecutor()) {
    std::unique_lock<std::mutex> lck(exit_mutex_);
    auto all_exited = [this] { return running_threads_ == 0; };
    if (shutdown_grace_ms_ < 0) {
      exit_cv_.wait(lck, all_exited);
    } else if (!exit_cv_.wait_for(lck, std::chrono::milliseconds(shutdown_grace_ms_), all_exited)) {
      lck.unlock();
      MW_WARN("Async pool executor '{}' still busy {} ms after shutdown, stopping it; "
              "tasks and timers still queued are discarded (see 'shutdown_grace_ms').",
              name_, shutdown_grace_ms_);
      io_ptr_->stop();
    }
  }

  for (auto itr = threads_.begin(); itr != threads_.end();) {
    if (itr->joinable()) itr->join();
    threads_.erase(itr++);
  }
}

bool ThreadPoolExecutor::IsInCurrentExecutor() const noexcept {
  return tl_current_executor_ == this;
}

void ThreadPoolExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  auto cur_state = state_.load(std::memory_order_acquire);
  if (cur_state != State::kInit && cur_state != State::kStart) [[unlikely]] {
    MW_ERROR("Async pool executor '{}' can only execute task when state is 'Init' or 'Start'.", name_);
    return;
  }

  try {
    asio::post(*io_ptr_, [task{std::move(task)}]() mutable {
      auto run_task = std::move(task);
      if (run_task) {
        run_task();
      }
    });
  } catch (const std::exception& e) {
    MW_ERROR("Async pool executor '{}' execute Task get exception: {}", name_, e.what());
  }
}

std::chrono::system_clock::time_point ThreadPoolExecutor::Now() const noexcept {
  if (!options_.use_system_clock) {
    return start_sys_tp_ +
           std::chrono::duration_cast<std::chrono::system_clock::time_point::duration>(
               std::chrono::steady_clock::now() - start_std_tp_);
  }

  return std::chrono::system_clock::now();
}

void ThreadPoolExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  auto cur_state = state_.load(std::memory_order_acquire);
  if (cur_state != State::kInit && cur_state != State::kStart) [[unlikely]] {
    MW_ERROR("Async pool executor '{}' can only execute task when state is 'Init' or 'Start'.",
                name_);
    return;
  }

  try {
    if (!options_.use_system_clock) {
      auto timer_ptr = std::make_shared<asio::steady_timer>(*io_ptr_);
      timer_ptr->expires_after(tp - Now());
      timer_ptr->async_wait([this, timer_ptr,
                             task{std::move(task)}](asio::error_code ec) mutable {
        if (ec) [[unlikely]] {
          MW_ERROR("Async pool executor '{}' timer get err, code '{}', msg: {}",
                      Name(), ec.value(), ec.message());
          return;
        }

        auto diff_time = std::chrono::steady_clock::now() - timer_ptr->expiry();

        if (options_.timeout_alarm_interval_ms >= 0 && diff_time > options_.timeout_alarm_threshold_us) [[unlikely]] {
          MW_WARN_INTERVAL(
              static_cast<uint64_t>(options_.timeout_alarm_interval_ms),
              "Async pool executor '{}' timer delay too much, error time value '{}', require '{}'. "
              "Perhaps the CPU load is too high",
              Name(), std::chrono::duration_cast<std::chrono::microseconds>(diff_time),
              options_.timeout_alarm_threshold_us);
        }

        auto run_task = std::move(task);
        if (run_task) {
          run_task();
        }
      });
    } else {
      auto timer_ptr = std::make_shared<asio::system_timer>(*io_ptr_);
      timer_ptr->expires_at(tp);
      timer_ptr->async_wait([this, timer_ptr,
                             task{std::move(task)}](asio::error_code ec) mutable {
        if (ec) [[unlikely]] {
          MW_ERROR("Async pool executor '{}' timer get err, code '{}', msg: {}",
                      Name(), ec.value(), ec.message());
          return;
        }

        auto diff_time = std::chrono::system_clock::now() - timer_ptr->expiry();

        if (options_.timeout_alarm_interval_ms >= 0 && diff_time > options_.timeout_alarm_threshold_us) [[unlikely]] {
          MW_WARN_INTERVAL(
              static_cast<uint64_t>(options_.timeout_alarm_interval_ms),
              "Async pool executor '{}' timer delay too much, error time value '{}', require '{}'. "
              "Perhaps the CPU load is too high",
              Name(), std::chrono::duration_cast<std::chrono::microseconds>(diff_time),
              options_.timeout_alarm_threshold_us);
        }

        auto run_task = std::move(task);
        if (run_task) {
          run_task();
        }
      });
    }
  } catch (const std::exception& e) {
    MW_ERROR("Async pool executor '{}' execute Task get exception: {}", name_, e.what());
  }
}

}  // namespace ibmw::runtime::core::executor
