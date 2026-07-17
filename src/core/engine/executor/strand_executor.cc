// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/executor/strand_executor.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::executor::StrandExecutor::Options> {
  using Options = ibmw::runtime::core::executor::StrandExecutor::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["bind_thread_pool_executor_name"] = rhs.bind_thread_pool_executor_name;
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

    if (node["bind_thread_pool_executor_name"])
      rhs.bind_thread_pool_executor_name =
          node["bind_thread_pool_executor_name"].as<std::string>();
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

void StrandExecutor::Initialize(std::string_view name,
                                    YAML::Node options_node) {
  MW_REQUIRE(get_asio_handle_,
                          "Get asio handle is not set before initialize.");

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "StrandExecutor can only be initialized once.");

  name_ = std::string(name);

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  start_sys_tp_ = std::chrono::system_clock::now();
  start_std_tp_ = std::chrono::steady_clock::now();

  MW_REQUIRE(
      !options_.bind_thread_pool_executor_name.empty(),
      "Invalide bind async pool executor name, name is empty.");

  auto* io_ptr = get_asio_handle_(options_.bind_thread_pool_executor_name);

  MW_REQUIRE(
      io_ptr,
      "Invalide bind async pool executor name, can not get asio io context.");

  strand_ptr_ = std::make_unique<Strand>(asio::make_strand(*io_ptr));

  options_node = options_;
}

void StrandExecutor::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void StrandExecutor::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;
}

void StrandExecutor::Dispatch(ibmw::executor::Task&& task) noexcept {
  if (state_.load() != State::kInit && state_.load() != State::kStart) [[unlikely]] {
    MW_ERROR("Serial queue executor '{}' can only execute task when state is 'Init' or 'Start'.",
                name_);
    return;
  }

  try {
    asio::post(*strand_ptr_, std::move(task));
  } catch (const std::exception& e) {
    MW_ERROR("Serial queue executor '{}' execute Task get exception: {}", name_, e.what());
  }
}

std::chrono::system_clock::time_point StrandExecutor::Now() const noexcept {
  if (!options_.use_system_clock) {
    return start_sys_tp_ +
           std::chrono::duration_cast<std::chrono::system_clock::time_point::duration>(
               std::chrono::steady_clock::now() - start_std_tp_);
  }

  return std::chrono::system_clock::now();
}

void StrandExecutor::DispatchAt(
    std::chrono::system_clock::time_point tp, ibmw::executor::Task&& task) noexcept {
  if (state_.load() != State::kInit && state_.load() != State::kStart) [[unlikely]] {
    MW_ERROR("Serial queue executor '{}' can only execute task when state is 'Init' or 'Start'.", name_);
    return;
  }

  try {
    if (!options_.use_system_clock) {
      auto timer_ptr = std::make_shared<asio::steady_timer>(*strand_ptr_);
      timer_ptr->expires_after(tp - Now());
      timer_ptr->async_wait([this, timer_ptr,
                             task{std::move(task)}](asio::error_code ec) {
        if (ec) [[unlikely]] {
          MW_ERROR("Serial queue executor '{}' timer get err, code '{}', msg: {}",
                      Name(), ec.value(), ec.message());
          return;
        }

        auto diff_time = std::chrono::steady_clock::now() - timer_ptr->expiry();
        if (options_.timeout_alarm_interval_ms >= 0 && diff_time > options_.timeout_alarm_threshold_us) [[unlikely]] {
          MW_WARN_INTERVAL(
              static_cast<uint64_t>(options_.timeout_alarm_interval_ms),
              "Serial queue executor '{}' timer delay too much, error time value '{}', require '{}'. "
              "Perhaps the CPU load is too high",
              Name(), std::chrono::duration_cast<std::chrono::microseconds>(diff_time),
              options_.timeout_alarm_threshold_us);
        }

        task();
      });
    } else {
      auto timer_ptr = std::make_shared<asio::system_timer>(*strand_ptr_);
      timer_ptr->expires_at(tp);
      timer_ptr->async_wait([this, timer_ptr,
                             task{std::move(task)}](asio::error_code ec) {
        if (ec) [[unlikely]] {
          MW_ERROR("Serial queue executor '{}' timer get err, code '{}', msg: {}",
                      Name(), ec.value(), ec.message());
          return;
        }

        auto diff_time = std::chrono::system_clock::now() - timer_ptr->expiry();

        if (options_.timeout_alarm_interval_ms >= 0 && diff_time > options_.timeout_alarm_threshold_us) [[unlikely]] {
          MW_WARN_INTERVAL(
              static_cast<uint64_t>(options_.timeout_alarm_interval_ms),
              "Serial queue executor '{}' timer delay too much, error time value '{}', require '{}'. "
              "Perhaps the CPU load is too high",
              Name(), std::chrono::duration_cast<std::chrono::microseconds>(diff_time),
              options_.timeout_alarm_threshold_us);
        }

        task();
      });
    }
  } catch (const std::exception& e) {
    MW_ERROR("Serial queue executor '{}' execute Task get exception: {}", name_, e.what());
  }
}

void StrandExecutor::RegisterGetAsioHandle(GetAsioHandle&& handle) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_asio_handle_ = std::move(handle);
}

}  // namespace ibmw::runtime::core::executor
