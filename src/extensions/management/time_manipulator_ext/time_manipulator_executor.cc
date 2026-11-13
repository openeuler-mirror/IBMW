// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "time_manipulator_ext/time_manipulator_executor.h"

#include <cmath>
#include <limits>

#include "engine/thread_tools.h"
#include "time_manipulator_ext/global.h"
#include "base/time_util.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::time_manipulator_extension::TimeManipulatorExecutor::Options> {
  using Options = ibmw::extensions::time_manipulator_extension::TimeManipulatorExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["bind_executor"] = rhs.bind_executor;
    node["dt_us"] = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(rhs.dt).count());
    node["init_ratio"] = rhs.init_ratio;
    node["max_ratio"] = rhs.max_ratio;
    node["wheel_size"] = rhs.wheel_size;
    node["thread_sched_policy"] = rhs.thread_sched_policy;
    node["thread_bind_cpu"] = rhs.thread_bind_cpu;
    node["use_system_clock"] = rhs.use_system_clock;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    rhs.bind_executor = node["bind_executor"].as<std::string>();

    if (node["dt_us"])
      rhs.dt = std::chrono::microseconds(node["dt_us"].as<uint64_t>());
    if (node["init_ratio"])
      rhs.init_ratio = node["init_ratio"].as<double>();
    if (node["max_ratio"])
      rhs.max_ratio = node["max_ratio"].as<double>();
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

namespace ibmw::extensions::time_manipulator_extension {

void TimeManipulatorExecutor::Initialize(std::string_view name,
                                         YAML::Node options_node) {
  MW_REQUIRE(
      get_executor_func_,
      "Get executor function is not set before initialize.");

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "TimeManipulatorExecutor can only be initialized once.");

  name_ = std::string(name);

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  MW_REQUIRE(
      !options_.bind_executor.empty(),
      "Invalide bind executor name, name is empty.");

  bind_executor_ref_ = get_executor_func_(options_.bind_executor);

  MW_REQUIRE(
      bind_executor_ref_,
      "Can not get executor {}.", options_.bind_executor);

  thread_safe_ = bind_executor_ref_.ThreadSafe();

  MW_REQUIRE(std::isfinite(options_.max_ratio) && options_.max_ratio >= 1.0 &&
                 options_.max_ratio < static_cast<double>(std::numeric_limits<uint32_t>::max()),
             "Time manipulator executor '{}': max_ratio must be in [1, 2^32-1), got {}.", name_,
             options_.max_ratio);
  MW_REQUIRE(SetTimeRatio(options_.init_ratio),
             "Time manipulator executor '{}': invalid init_ratio {} (max_ratio {}).", name_,
             options_.init_ratio, options_.max_ratio);

  dt_count_ = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(options_.dt).count());

  // Same constraints as the core TimerExecutor: dt and wheel sizes are divisors.
  MW_REQUIRE(dt_count_ > 0, "Time manipulator executor '{}': dt_us must be greater than 0.", name_);
  MW_REQUIRE(!options_.wheel_size.empty(),
             "Time manipulator executor '{}': wheel_size must not be empty.", name_);
  for (size_t sz : options_.wheel_size) {
    MW_REQUIRE(sz >= 2, "Time manipulator executor '{}': every wheel_size entry must be at least 2.", name_);
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

void TimeManipulatorExecutor::Start() {
  // EXT-017: the start point is known before any thread reads it; it is
  // set together with the state, so DispatchAt() never sees kStart without it.
  const auto start_sys_tp = std::chrono::system_clock::now();
  const auto start_std_tp = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::shared_mutex> lck(tick_mutex_);
    MW_REQUIRE(
        std::atomic_exchange(&state_, State::kStart) == State::kInit,
        "Method can only be called when state is 'Init'.");
    start_time_point_.store(ibmw::common::util::GetTimestampNs(start_sys_tp), std::memory_order_relaxed);
  }

  timer_thread_ptr_ = std::make_unique<std::thread>(
      [this, start_sys_tp, start_std_tp] { TimerLoop(start_sys_tp, start_std_tp); });

  start_flag_.wait(false);
}

void TimeManipulatorExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  {
    // A paused timer thread checks the state under this lock.
    std::unique_lock<std::shared_mutex> lck(ratio_mutex_);
  }
  pause_cv_.notify_all();

  if (timer_thread_ptr_ && timer_thread_ptr_->joinable())
    timer_thread_ptr_->join();

  timer_thread_ptr_.reset();
  {
    // DispatchAt() checks the state under this lock.
    std::unique_lock<std::shared_mutex> lck(tick_mutex_);
    timing_task_map_.clear();
    timing_wheel_vec_.clear();
  }
  get_executor_func_ = nullptr;
}

bool TimeManipulatorExecutor::IsInCurrentExecutor() const noexcept {
  try {
    return bind_executor_ref_.IsInCurrentExecutor();
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
  return false;
}

void TimeManipulatorExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  try {
    bind_executor_ref_.Dispatch(std::move(task));
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

std::chrono::system_clock::time_point TimeManipulatorExecutor::Now() const noexcept {
  std::shared_lock<std::shared_mutex> lck(tick_mutex_);

  return ibmw::common::util::GetTimePointFromTimestampNs(
      current_tick_count_ * dt_count_ + start_time_point_.load(std::memory_order_relaxed));
}

void TimeManipulatorExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  try {
    const uint64_t tp_ns = ibmw::common::util::GetTimestampNs(tp);
    std::unique_lock<std::shared_mutex> lck(tick_mutex_);

    if (state_.load(std::memory_order_relaxed) != State::kStart) [[unlikely]] {
      // Before Start() there is no virtual time origin yet.
      MW_WARN("Time manipulator executor '{}' is not running, timed task dropped.", name_);
      return;
    }
    const uint64_t start_ns = start_time_point_.load(std::memory_order_relaxed);

    // EXT-017: compare before subtracting; a time point before the start is due.
    if (tp_ns < start_ns || tp_ns - start_ns < current_tick_count_ * dt_count_) {
      lck.unlock();
      bind_executor_ref_.Dispatch(std::move(task));
      return;
    }

    const uint64_t target_tick = (tp_ns - start_ns) / dt_count_;

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
        // Unsorted on purpose: each level 0 slot holds one tick, so tasks still
        // run at their tick; the order within one tick is not guaranteed.
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

void TimeManipulatorExecutor::RegisterGetExecutorFunc(
    const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  get_executor_func_ = get_executor_func;
}

bool TimeManipulatorExecutor::SetTimeRatio(double ratio) {
  // Checked before any cast: converting NaN, infinity or a value out of the
  // uint32_t range is undefined behavior.
  if (std::isnan(ratio) || std::isinf(ratio) || ratio > options_.max_ratio) [[unlikely]] {
    MW_WARN("Time manipulator executor '{}': reject time ratio {} (max_ratio {}).", name_, ratio,
            options_.max_ratio);
    return false;
  }

  // 0, negative or too small: pause
  uint32_t real_ratio = std::numeric_limits<uint32_t>::max();
  if (ratio >= 1.0) {
    // 1 or more, fast forward
    real_ratio = static_cast<uint32_t>(std::lround(ratio));
  } else if (ratio > 1e-15 && (1.0 / ratio) < std::numeric_limits<uint32_t>::max() - 1.0) {
    // Greater than 0 and less than 1, slow release
    real_ratio = static_cast<uint32_t>(std::lround(1.0 / ratio));
  }

  {
    std::unique_lock<std::shared_mutex> lck(ratio_mutex_);
    ratio_direction_ = ratio >= 1.0;
    real_ratio_ = real_ratio;
  }
  pause_cv_.notify_all();
  return true;
}

double TimeManipulatorExecutor::GetTimeRatio() const {
  std::shared_lock<std::shared_mutex> lck(ratio_mutex_);

  if (ratio_direction_)
    return real_ratio_;

  if (real_ratio_ == std::numeric_limits<uint32_t>::max())
    return 0.0;

  return 1.0 / real_ratio_;
}

void TimeManipulatorExecutor::TimerLoop(std::chrono::system_clock::time_point start_sys_tp,
                                        std::chrono::steady_clock::time_point start_std_tp) {
  std::string threadname = name_;

  try {
    ibmw::runtime::core::util::SetNameForCurrentThread(threadname);
    ibmw::runtime::core::util::BindCpuForCurrentThread(options_.thread_bind_cpu);
    ibmw::runtime::core::util::SetCpuSchedForCurrentThread(options_.thread_sched_policy);
  } catch (const std::exception& e) {
    MW_WARN("Set thread policy for time manipulator executor '{}' get exception, {}",
               Name(), e.what());
  }

  auto last_loop_sys_tp = start_sys_tp;
  auto last_loop_std_tp = start_std_tp;

  start_flag_.store(true);
  start_flag_.notify_all();

  while (state_.load() != State::kShutdown) {
    try {
      // Obtain the time ratio. Note: Speed adjustment can only take effect on the next tick
      ratio_mutex_.lock_shared();

      bool ratio_direction = ratio_direction_;
      uint32_t real_ratio = real_ratio_;

      ratio_mutex_.unlock_shared();

      // sleep a tick
      if (real_ratio == std::numeric_limits<uint32_t>::max()) {
        // Paused until a new ratio or shutdown; the next tick counts from then.
        {
          std::shared_lock<std::shared_mutex> lck(ratio_mutex_);
          pause_cv_.wait(lck, [this] {
            return state_.load() == State::kShutdown ||
                   real_ratio_ != std::numeric_limits<uint32_t>::max();
          });
        }
        last_loop_sys_tp = std::chrono::system_clock::now();
        last_loop_std_tp = std::chrono::steady_clock::now();

        continue;
      }

      auto real_dt = ratio_direction ? options_.dt : (options_.dt * real_ratio);
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

        // A long sleep ends at once on shutdown; a tick at the default dt stays
        // a plain sleep. Only shutdown ends it: a new ratio applies from the next tick.
        const auto sleep_until = [&](const auto& time_point) {
          static constexpr auto kMinWakeableSleepDt = std::chrono::milliseconds(500);
          if (sleep_time < kMinWakeableSleepDt) {
            std::this_thread::sleep_until(time_point);
            return;
          }
          std::shared_lock<std::shared_mutex> lck(ratio_mutex_);
          pause_cv_.wait_until(lck, time_point,
                               [this] { return state_.load() == State::kShutdown; });
        };

        if (!options_.use_system_clock) {
          sleep_until(
              last_loop_std_tp +=
              std::chrono::duration_cast<std::chrono::steady_clock::time_point::duration>(sleep_time));
        } else {
          sleep_until(
              last_loop_sys_tp +=
              std::chrono::duration_cast<std::chrono::system_clock::time_point::duration>(sleep_time));
        }

      } while (state_.load() != State::kShutdown && real_dt.count());

      // The sleep may have ended early: this tick's tasks are not due.
      if (state_.load() == State::kShutdown) break;

      // Advance the time wheel

      // Number of ticks to go
      uint64_t diff_tick_count = ratio_direction ? real_ratio : 1;

      tick_mutex_.lock();

      do {
        // Take out the task
        TaskList task_list = timing_wheel_vec_[0].Tick();

        // Update time point before the lock is released, as the core
        // TimerExecutor does: a DispatchAt() from a running task must not see
        // the tick whose slot was just taken out.
        ++current_tick_count_;

        // Perform tasks
        if (!task_list.empty()) {
          tick_mutex_.unlock();

          for (auto& itr : task_list) {
            bind_executor_ref_.Dispatch(std::move(itr.task));
          }

          tick_mutex_.lock();
        }

      } while (--diff_tick_count);

      tick_mutex_.unlock();
    } catch (const std::exception& e) {
      MW_FATAL("Time manipulator executor '{}' run loop get exception, {}",
                  Name(), e.what());
    }
  }
}
}  // namespace ibmw::extensions::time_manipulator_extension
