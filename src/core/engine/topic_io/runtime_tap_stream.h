// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "base/log_util.h"
#include "engine/topic_io/runtime_tap_source.h"
#include "engine/runtime_graph/runtime_graph_client.h"
#include "extension_api/ibmw_extension_runtime.h"

namespace ibmw::runtime::core::topic_io {

using ibmw::runtime::core::runtime_graph::RuntimeGraphAgentClient;
using ibmw::runtime::core::runtime_graph::TapSessionDescriptor;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;

// The host supplies message views; the stream owns queued serialized payloads.
class PublishTapCapability {
 public:
  using Callback = std::function<void(ibmw::extension::MessageView&)>;
  virtual ~PublishTapCapability() = default;
  virtual uint64_t Register(Callback callback) = 0;
  virtual bool Unregister(uint64_t handle) = 0;
};

class RuntimeTapServer {
 public:
  RuntimeTapServer(
      PublishTapCapability& publish_tap,
      std::string runtime_id,
      std::filesystem::path socket_directory,
      ibmw::common::util::LoggerWrapper logger = {});
  ~RuntimeTapServer();

  RuntimeTapServer(const RuntimeTapServer&) = delete;
  RuntimeTapServer& operator=(const RuntimeTapServer&) = delete;

  void Start();
  TapSessionDescriptor StartSession(
      const TopicAdvertisement& advertisement);
  bool StopSession(std::string_view session_id);
  void Shutdown();

 private:
  struct Session;

  void OnPublish(ibmw::extension::MessageView& frame);

  PublishTapCapability& publish_tap_;
  const std::string runtime_id_;
  const std::filesystem::path socket_directory_;
  ibmw::common::util::LoggerWrapper logger_;
  std::atomic<bool> started_{false};
  uint64_t publish_tap_handle_ = 0;
  std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::unordered_multimap<std::string, std::shared_ptr<Session>> sessions_by_publisher_;
  uint64_t next_session_id_ = 1;
};

class UnixRuntimeTapClient final : public RuntimeTapClient {
 public:
  using DroppedMessageCallback = std::function<void(uint64_t)>;
  using StreamErrorCallback = std::function<void(const std::string&)>;

  explicit UnixRuntimeTapClient(
      std::string control_socket,
      DroppedMessageCallback dropped_message_callback = {},
      StreamErrorCallback stream_error_callback = {},
      ibmw::common::util::LoggerWrapper logger = {});
  ~UnixRuntimeTapClient() override;

  void StartTap(
      const TopicAdvertisement& advertisement,
      ibmw::transport::SerializedMessageCallback callback)
      override;
  void StopTap() override;

 private:
  void ReadLoop(
      TopicAdvertisement advertisement,
      ibmw::transport::SerializedMessageCallback callback);
  void ReportDroppedTotal(uint64_t dropped_total);
  void ReportStreamError(std::string error);

  const std::string control_socket_;
  RuntimeGraphAgentClient agent_client_;
  DroppedMessageCallback dropped_message_callback_;
  StreamErrorCallback stream_error_callback_;
  ibmw::common::util::LoggerWrapper logger_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> terminal_received_{false};
  std::atomic<bool> remote_closed_{false};
  std::atomic<int> stream_fd_{-1};
  std::string session_id_;
  std::thread reader_thread_;
  uint64_t reported_dropped_total_ = 0;
  std::mutex stream_error_mutex_;
  std::optional<std::string> stream_error_;
};

}  // namespace ibmw::runtime::core::topic_io
