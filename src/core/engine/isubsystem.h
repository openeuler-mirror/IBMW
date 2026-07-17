// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// ISubsystem — abstract base class for all IBMW engine subsystems.
//
// Consolidates the boilerplate shared by every subsystem:
//   • Lifecycle state machine (PreInit → Init → Start → Shutdown)
//   • Logger injection (SetLogger / GetLogger)
//   • Optional initialization-report generation
//
// Concrete subsystems inherit publicly and implement
// Initialize(YAML::Node) / Start() / Shutdown().

#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "base/log_util.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core {

class ISubsystem {
 public:
  // Lifecycle state — identical across all subsystems.
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  ISubsystem()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}

  virtual ~ISubsystem() = default;

  ISubsystem(const ISubsystem&) = delete;
  ISubsystem& operator=(const ISubsystem&) = delete;

  // --- Lifecycle ------------------------------------------------------------

  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  // --- State query ----------------------------------------------------------

  State GetState() const { return state_.load(); }

  // --- Initialization report (optional) -------------------------------------

  virtual std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const { return {}; }

  // --- Logger injection -----------------------------------------------------

  void SetLogger(
      const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) {
    logger_ptr_ = logger_ptr;
  }

  const ibmw::common::util::LoggerWrapper& GetLogger() const {
    return *logger_ptr_;
  }

 protected:
  std::atomic<State> state_{State::kPreInit};
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;
};

}  // namespace ibmw::runtime::core
