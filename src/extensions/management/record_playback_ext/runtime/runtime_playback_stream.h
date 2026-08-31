// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "cpp/transport/serialized_message.h"
#include "engine/runtime_graph/runtime_graph_client.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::RuntimeGraphAgentClient;
using ibmw::runtime::core::runtime_graph::TapSessionDescriptor;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

class RuntimePlaybackServer {
 public:
  using MessageHandler = std::function<void(
      const TopicAdvertisement&,
      ibmw::transport::SerializedMessage&&)>;

  RuntimePlaybackServer(
      std::string runtime_id,
      std::filesystem::path socket_directory,
      MessageHandler message_handler);
  ~RuntimePlaybackServer();

  RuntimePlaybackServer(const RuntimePlaybackServer&) = delete;
  RuntimePlaybackServer& operator=(const RuntimePlaybackServer&) = delete;

  void Start();
  TapSessionDescriptor StartSession(
      const TopicAdvertisement& advertisement);
  bool StopSession(std::string_view session_id);
  void Shutdown();

 private:
  struct Session;

  const std::string runtime_id_;
  const std::filesystem::path socket_directory_;
  MessageHandler message_handler_;
  std::atomic<bool> started_{false};
  std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  uint64_t next_session_id_ = 1;
};

class UnixRuntimePlaybackClient {
 public:
  explicit UnixRuntimePlaybackClient(std::string control_socket);
  ~UnixRuntimePlaybackClient();

  UnixRuntimePlaybackClient(const UnixRuntimePlaybackClient&) = delete;
  UnixRuntimePlaybackClient& operator=(const UnixRuntimePlaybackClient&) =
      delete;

  void Start(const TopicAdvertisement& advertisement);
  void Publish(
      const ibmw::transport::SerializedMessage& message);
  void Stop();

 private:
  RuntimeGraphAgentClient agent_client_;
  std::atomic<int> stream_fd_{-1};
  std::string session_id_;
};

}  // namespace ibmw::extensions::record_playback_extension
