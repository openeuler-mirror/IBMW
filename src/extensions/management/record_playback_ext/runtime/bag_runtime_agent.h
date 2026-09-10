// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "record_playback_ext/runtime/runtime_registration.h"

namespace ibmw::extensions::record_playback_extension {

struct TapSessionDescriptor {
  std::string session_id;
  std::string stream_socket;
};

class BagRuntimeAgent {
 public:
  struct Options {
    RuntimeRegistration registration;
    std::filesystem::path registry_path = "/tmp/ibmw/runtimes";
  };

  using StartTapHandler =
      std::function<TapSessionDescriptor(const TopicAdvertisement&)>;
  using StopTapHandler = std::function<bool(std::string_view session_id)>;
  using StartPlaybackHandler =
      std::function<TapSessionDescriptor(const TopicAdvertisement&)>;
  using StopPlaybackHandler =
      std::function<bool(std::string_view session_id)>;

  explicit BagRuntimeAgent(Options options);
  ~BagRuntimeAgent();

  BagRuntimeAgent(const BagRuntimeAgent&) = delete;
  BagRuntimeAgent& operator=(const BagRuntimeAgent&) = delete;

  void SetStartTapHandler(StartTapHandler handler);
  void SetStopTapHandler(StopTapHandler handler);
  void SetStartPlaybackHandler(StartPlaybackHandler handler);
  void SetStopPlaybackHandler(StopPlaybackHandler handler);

  void Initialize();
  void Start();
  void Shutdown();

  const RuntimeRegistration& Registration() const noexcept {
    return options_.registration;
  }

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
};

}  // namespace ibmw::extensions::record_playback_extension
