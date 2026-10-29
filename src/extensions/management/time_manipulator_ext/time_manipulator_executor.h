// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "cpp/executor/executor.h"
#include "engine/executor/executor_base.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::time_manipulator_extension {

class TimeManipulatorExecutor : public ibmw::runtime::core::executor::ExecutorBase {
 public:
  struct Options {
    std::string bind_executor;
    std::chrono::nanoseconds dt = std::chrono::microseconds(100000);  // 100 ms
    double init_ratio = 1.0;
    std::vector<size_t> wheel_size = {100, 360};  // 1 h
    std::string thread_sched_policy;
    std::vector<uint32_t> thread_bind_cpu;
    // Clock the ticks are paced by. Virtual time always starts at the
    // system time of Start(), so Now() and DispatchAt() use the system clock
    // domain either way.
    bool use_system_clock = false;
  };

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  TimeManipulatorExecutor() = default;
  ~TimeManipulatorExecutor() override = default;

  void Initialize(std::string_view name, YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  std::string_view Type() const noexcept override { return "time_manipulator"; }
  std::string_view Name() const noexcept override { return name_; }

  bool ThreadSafe() const noexcept override { return thread_safe_; }
  bool IsInCurrentExecutor() const noexcept override;
  bool SupportsTimedDispatch() const noexcept override { return true; }

  void Dispatch(ibmw::executor::Task&& task) noexcept override;

  std::chrono::system_clock::time_point Now() const noexcept override;
  void DispatchAt(std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept override;

  State GetState() const { return state_.load(); }

  void RegisterGetExecutorFunc(
      const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func);

  void SetTimeRatio(double ratio);
  double GetTimeRatio() const;

 private:
  void TimerLoop(std::chrono::system_clock::time_point start_sys_tp,
                 std::chrono::steady_clock::time_point start_std_tp);

  struct TaskWithTimestamp {
    uint64_t tick_count;  // Tick count since start_time
    ibmw::executor::Task task;
  };

  using TaskList = std::list<TaskWithTimestamp>;

  struct TimingWheelTool {
    uint64_t current_pos;
    uint64_t scale;
    uint64_t unit;   // ticks per slot (the scale of the level below)
    uint64_t limit;  // first tick that does not fit into this level yet (levels >= 1)
    std::vector<TaskList> wheel;
    std::function<void()> borrow_func;

    TaskList Tick() {
      TaskList task_list;
      task_list.swap(wheel[current_pos]);

      ++current_pos;
      if (current_pos == wheel.size()) [[unlikely]] {
        current_pos = 0;
        borrow_func();
      }

      return task_list;
    }
  };

 private:
  std::string name_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  std::function<ibmw::executor::ExecutorHandle(std::string_view)> get_executor_func_;
  executor::ExecutorHandle bind_executor_ref_;
  bool thread_safe_ = true;

  uint64_t dt_count_;

  mutable std::shared_mutex ratio_mutex_;
  bool ratio_direction_ = true;
  uint32_t real_ratio_ = 1;

  // Set before the timer thread starts; read by DispatchAt() from any thread.
  std::atomic<uint64_t> start_time_point_ = 0;
  std::atomic_bool start_flag_ = false;

  mutable std::shared_mutex tick_mutex_;
  uint64_t current_tick_count_ = 0;
  std::vector<TimingWheelTool> timing_wheel_vec_;
  uint64_t timing_task_map_pos_ = 0;
  std::unordered_map<uint64_t, TaskList> timing_task_map_;
  uint64_t timing_task_map_unit_ = 1;  // ticks per far map key

  std::unique_ptr<std::thread> timer_thread_ptr_;
};

}  // namespace ibmw::extensions::time_manipulator_extension
