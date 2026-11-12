// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/runtime_graph/runtime_registration.h"
#include "extension_api/ibmw_module_lifecycle.h"

namespace ibmw::runtime::core::runtime_graph {

using ibmw::extension::TapSessionDescriptor;

class RuntimeGraphAgent {
 public:
  struct Options {
    RuntimeRegistration registration;
    std::filesystem::path registry_path = ibmw::extension::DefaultRuntimeRegistryPath();
  };

  using StartTapHandler = ibmw::extension::RuntimeGraphV1::StartTapHandler;
  using StopTapHandler = ibmw::extension::RuntimeGraphV1::StopTapHandler;
  using StartPlaybackHandler = ibmw::extension::RuntimeGraphV1::StartPlaybackHandler;
  using StopPlaybackHandler = ibmw::extension::RuntimeGraphV1::StopPlaybackHandler;
  using ListModuleLifecycleHandler =
      std::function<std::vector<ibmw::extension::ModuleLifecycleStatus>()>;

  explicit RuntimeGraphAgent(Options options);
  ~RuntimeGraphAgent();

  RuntimeGraphAgent(const RuntimeGraphAgent&) = delete;
  RuntimeGraphAgent& operator=(const RuntimeGraphAgent&) = delete;

  void SetStartTapHandler(StartTapHandler handler);
  void SetStopTapHandler(StopTapHandler handler);
  void SetStartPlaybackHandler(StartPlaybackHandler handler);
  void SetStopPlaybackHandler(StopPlaybackHandler handler);
  void SetListModuleLifecycleHandler(ListModuleLifecycleHandler handler);

  void Initialize();
  void Start();
  void Shutdown();

  const RuntimeRegistration& Registration() const noexcept {
    return options_.registration;
  }

  // Before Start only; initialized registrations retain their process identity.
  void UpdateRegistration(RuntimeRegistration registration);

 private:
  enum class State {
    kCreated,
    kInitialized,
    kRunning,
    kStopped,
  };

  void Serve();
  void HandleClient(int client_fd);
  nlohmann::json HandleRequest(const nlohmann::json& request);
  TopicAdvertisement FindTopic(const nlohmann::json& params) const;

  Options options_;
  std::filesystem::path registration_path_;
  std::atomic<State> state_ = State::kCreated;
  std::atomic<int> server_fd_ = -1;
  std::atomic<int> client_fd_ = -1;
  std::thread server_thread_;
  StartTapHandler start_tap_handler_;
  StopTapHandler stop_tap_handler_;
  StartPlaybackHandler start_playback_handler_;
  StopPlaybackHandler stop_playback_handler_;
  ListModuleLifecycleHandler list_module_lifecycle_handler_;
};

}  // namespace ibmw::runtime::core::runtime_graph
