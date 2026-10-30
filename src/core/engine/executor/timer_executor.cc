// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/timer_executor.h"
#include "engine/thread_tools.h"
#include "base/time_util.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::TimerExecutor::Options> {
  using Options = ibmw::runtime::core::executor::TimerExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["bind_executor"] = rhs.bind_executor;
    node["dt_us"] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(rhs.dt).count());
    node["wheel_size"] = rhs.wheel_size;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["use_system_clock"] = rhs.use_system_clock;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["bind_executor"])
      rhs.bind_executor = node["bind_executor"].as<std::string>();
    if (node["dt_us"])
      rhs.dt = std::chrono::microseconds(node["dt_us"].as<uint64_t>());
    if (node["wheel_size"])
      rhs.wheel_size = node["wheel_size"].as<std::vector<size_t>>();
    if (node["thread_sched_policy"])
      rhs.thread_sched_policy = node["thread_sched_policy"].as<std::string>();
    if (node["thread_bind_cpu"])
      rhs.thread_bind_cpu = node["thread_bind_cpu"].as<std::vector<uint32_t>>();
    if (node["use_system_clock"])
      rhs.use_system_clock = node["use_system_clock"].as<bool>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::executor {

void TimerExecutor::Initialize(std::string_view name,
                                   YAML::Node options_node) {
  MW_REQUIRE(
      get_executor_func_,
      "Get executor function is not set before initialize.");

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "TimerExecutor can only be initialized once.");

  name_ = std::string(name);

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  if (!options_.bind_executor.empty()) {
    bind_executor_ref_ = get_executor_func_(options_.bind_executor);

    MW_REQUIRE(
        bind_executor_ref_,
        "Can not get executor {}.", options_.bind_executor);

    MW_REQUIRE(
        bind_executor_ref_.Name() != Name(),
        "Bind executor '{}' is self!", options_.bind_executor);

    thread_safe_ = bind_executor_ref_.ThreadSafe();
  }

  dt_count_ = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(options_.dt).count());

  // dt_count_ and every wheel size are used as divisors on the dispatch path.
  MW_REQUIRE(dt_count_ > 0, "Timer executor '{}': dt_us must be greater than 0.", name_);
  MW_REQUIRE(!options_.wheel_size.empty(), "Timer executor '{}': wheel_size must not be empty.", name_);
  for (size_t sz : options_.wheel_size) {
    MW_REQUIRE(sz >= 2, "Timer executor '{}': every wheel_size entry must be at least 2.", name_);
  }

  uint64_t cur_scale = 1;
  for (size_t ii = 0; ii < options_.wheel_size.size(); ++ii) {
    uint64_t start_pos = (ii == 0) ? 0 : 1;
    const uint64_t unit = cur_scale;
    timing_wheel_vec_.emplace_back(TimingWheelTool{
        .current_pos = start_pos,
        .scale = (cur_scale *= options_.wheel_size[ii]),
        .unit = unit,
        // Slot 0 counts as pulled: the next wheel_size slots are free.
        .limit = (options_.wheel_size[ii] + 1) * unit,
        .wheel = std::vector<TaskList>(options_.wheel_size[ii]),
        .borrow_func = [ii, this]() {
          TaskList task_list;
          if (ii < options_.wheel_size.size() - 1) {
            // A slot of the level above is pulled down here, before it
            // starts (when every level below wraps): one more slot of it is
            // free from now on.
            auto& upper = timing_wheel_vec_[ii + 1];
            upper.limit += upper.unit;
            task_list = upper.Tick();
          } else {
            auto itr = timing_task_map_.find(timing_task_map_pos_);
            ++timing_task_map_pos_;
            if (itr != timing_task_map_.end()) {
              task_list = std::move(itr->second);
              timing_task_map_.erase(itr);
            }
          }

          // Slot of a task within this level: its tick in units of the
          // level below, as in DispatchAt().
          auto& level = timing_wheel_vec_[ii];
          while (!task_list.empty()) {
            auto itr = task_list.begin();
            auto& cur_list = level.wheel[(itr->tick_count / level.unit) % level.wheel.size()];
            cur_list.splice(cur_list.end(), task_list, itr);
          }
        }});
  }

  timing_task_map_pos_ = 1;
  timing_task_map_unit_ = cur_scale;

  options_node = options_;
}

void TimerExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  timer_thread_ptr_ = std::make_unique<std::thread>(std::bind(&TimerExecutor::TimerLoop, this));

  start_flag_.wait(false);
}

void TimerExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (timer_thread_ptr_ && timer_thread_ptr_->joinable())
    timer_thread_ptr_->join();

  timer_thread_ptr_.reset();
  // The dropped tasks are destroyed after the lock is released: a task's
  // destructor may call back into this executor.
  decltype(timing_task_map_) dropped_map;
  decltype(timing_wheel_vec_) dropped_wheels;
  {
    // DispatchAt() checks start_flag_ under this lock.
    std::unique_lock<std::shared_mutex> lck(tick_mutex_);
    start_flag_.store(false, std::memory_order_relaxed);
    dropped_map.swap(timing_task_map_);
    dropped_wheels.swap(timing_wheel_vec_);
  }
  get_executor_func_ = nullptr;
}

bool TimerExecutor::IsInCurrentExecutor() const noexcept {
  try {
    return bind_executor_ref_
               ? bind_executor_ref_.IsInCurrentExecutor()
               : (tid_.load(std::memory_order_relaxed) == std::this_thread::get_id());
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
  return false;
}

void TimerExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  try {
    if (bind_executor_ref_) {
      bind_executor_ref_.Dispatch(std::move(task));
      return;
    }

    std::unique_lock<std::mutex> lck(imd_mutex_);
    imd_queue_.emplace(std::move(task));
  } catch (const std::exception& e) {
    MW_ERROR("TimeWheel executor '{}' execute Task get exception: {}", name_, e.what());
  }
}

std::chrono::system_clock::time_point TimerExecutor::Now() const noexcept {
  std::shared_lock<std::shared_mutex> lck(tick_mutex_);

  return ibmw::common::util::GetTimePointFromTimestampNs(
      current_tick_count_ * dt_count_ + start_time_point_);
}

void TimerExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  try {
    const uint64_t tp_ns = ibmw::common::util::GetTimestampNs(tp);

    std::unique_lock<std::shared_mutex> lck(tick_mutex_);

    // Before the timer thread has set the time origin, a task would be
    // scheduled against origin 0 and never run; after Shutdown() (which
    // clears the flag under this lock) the wheels are gone. start_flag_
    // (acquire) publishes start_time_point_.
    if (!start_flag_.load(std::memory_order_acquire)) [[unlikely]] {
      WarnTimedTaskDropped();
      return;
    }

    // Compare before subtracting: a time point before the start is due.
    if (tp_ns < start_time_point_ || tp_ns - start_time_point_ < current_tick_count_ * dt_count_) {
      lck.unlock();
      Dispatch(std::move(task));
      return;
    }

    const uint64_t target_tick = (tp_ns - start_time_point_) / dt_count_;

    // Level 0 holds the next wheel_size[0] ticks. A slot of a higher level
    // is pulled one level down before it starts, so a task goes to the
    // first level whose slot for it is not pulled yet and comes up within
    // one rotation (target below the level's limit), otherwise to the far map.
    auto& level0 = timing_wheel_vec_[0];
    if (target_tick - current_tick_count_ < level0.wheel.size()) {
      level0.wheel[target_tick % level0.wheel.size()].emplace_back(TaskWithTimestamp{target_tick, std::move(task)});
      return;
    }
    for (size_t ii = 1; ii < timing_wheel_vec_.size(); ++ii) {
      auto& level = timing_wheel_vec_[ii];
      if (target_tick < level.limit) {
        // TODO: Sorting tasks based on time and inserting them into it
        level.wheel[(target_tick / level.unit) % level.wheel.size()].emplace_back(
            TaskWithTimestamp{target_tick, std::move(task)});
        return;
      }
    }

    timing_task_map_[target_tick / timing_task_map_unit_].emplace_back(
        TaskWithTimestamp{target_tick, std::move(task)});
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void TimerExecutor::WarnTimedTaskDropped() const noexcept {
  try {
    MW_WARN("Timer executor '{}' is not running, timed task dropped.", name_);
  } catch (...) {
  }
}

void TimerExecutor::RegisterGetExecutorFunc(
    const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  get_executor_func_ = get_executor_func;
}

void TimerExecutor::TimerLoop() {
  tid_.store(std::this_thread::get_id(), std::memory_order_relaxed);

  std::string threadname = name_;

  try {
    ibmw::runtime::core::util::SetNameForCurrentThread(threadname);
    ibmw::runtime::core::util::BindCpuForCurrentThread(options_.thread_bind_cpu);
    ibmw::runtime::core::util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
  } catch (const std::exception& e) {
    MW_WARN("Set thread policy for time manipulator executor '{}' get exception, {}",
               Name(), e.what());
  }

  auto last_loop_sys_tp = std::chrono::system_clock::now();
  auto last_loop_std_tp = std::chrono::steady_clock::now();

  // Record the initial time
  start_time_point_ =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          (options_.use_system_clock ? last_loop_sys_tp.time_since_epoch() : last_loop_std_tp.time_since_epoch()))
          .count();

  start_flag_.store(true);
  start_flag_.notify_all();

  while (state_.load() != State::kShutdown) {
    try {
      // sleep a dt
      auto real_dt = options_.dt;
      do {
        // Maximum sleep time
        static constexpr auto kMaxSleepDt = std::chrono::seconds(1);

        auto sleep_time = (real_dt > kMaxSleepDt) ? kMaxSleepDt : real_dt;
        real_dt -= sleep_time;

        // A small optimization to prevent real_dt from being too small
        if (real_dt.count() && options_.dt < kMaxSleepDt && real_dt <= options_.dt) {
          sleep_time += real_dt;
          real_dt = std::chrono::nanoseconds(0);
        }

        if (!options_.use_system_clock) {
          std::this_thread::sleep_until(
              last_loop_std_tp +=
              std::chrono::duration_cast<std::chrono::steady_clock::time_point::duration>(sleep_time));
        } else {
          std::this_thread::sleep_until(
              last_loop_sys_tp +=
              std::chrono::duration_cast<std::chrono::system_clock::time_point::duration>(sleep_time));
        }

      } while (state_.load() != State::kShutdown && real_dt.count());

      // Perform an immediate task
      if (!bind_executor_ref_) {
        std::queue<ibmw::executor::Task> tmp_queue;

        imd_mutex_.lock();
        imd_queue_.swap(tmp_queue);
        imd_mutex_.unlock();

        while (!tmp_queue.empty()) {
          auto& task = tmp_queue.front();

          try {
            task();
          } catch (const std::exception& e) {
            MW_FATAL("Time wheel executor run task get exception, {}", e.what());
          }

          tmp_queue.pop();
        }
      }

      // Advance the time wheel by one tick
      tick_mutex_.lock();

      // Take out the task
      TaskList task_list = timing_wheel_vec_[0].Tick();

      // Update time point BEFORE dispatching tasks, so that Now() and
      // DispatchAt() see the current tick during task execution.
      ++current_tick_count_;

      tick_mutex_.unlock();

      // Perform tasks (outside the lock)
      for (auto& itr : task_list) {
        if (bind_executor_ref_) {
          bind_executor_ref_.Dispatch(std::move(itr.task));
        } else {
          try {
            itr.task();
          } catch (const std::exception& e) {
            MW_FATAL("Time wheel executor run task get exception, {}", e.what());
          }
        }
      }
    } catch (const std::exception& e) {
      MW_FATAL("Time manipulator executor '{}' run loop get exception, {}",
                  Name(), e.what());
    }
  }
}
}  // namespace ibmw::runtime::core::executor
