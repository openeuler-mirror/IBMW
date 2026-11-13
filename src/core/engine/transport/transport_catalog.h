// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/transport/internal/callback_invocation_envelope.h"
#include "engine/transport/message_frame.h"
#include "engine/transport/topic_descriptor.h"
#ifdef IBMW_ENABLE_COMM_TRACE
#include "engine/transport/internal/comm_trace_send_overlay.h"
#endif
#include "base/log_util.h"

namespace ibmw::runtime::core::observability { struct MetricsEntry; struct PodMetricsEntry; struct CachedMetricsGate; }
namespace ibmw::runtime::core::transport {

// Forward decls for cached pointers in SendTypeWrapper (Phase 7 §7.2 C2).
class TransportDriver;
class FrameworkAsyncTransportFilterCollector;

using MessageCallback = std::function<void(
    MessageFrame&, std::function<void()>&&,
    const internal::CallbackInvocationEnvelopeV1*)>;

struct MessageHandlerWrapper {
  TransportInfo info;
  QosDescriptor requested_qos;
  std::unordered_set<std::string> require_cache_serialization_types;
  MessageCallback callback;
  uint64_t callback_registration_id = 0;
};

// Phase 7 §7.3 §U: receiver-side zero-copy fast lane handler.
//
// Symmetric with publisher-side LoanSample: the dispatcher uses this
// wrapper to hand a driver a callback that should be invoked directly
// with the driver's borrowed payload pointer (no MessageFrame, no
// deserialization, no filter chain). Drivers that override
// TransportDriver::AttachLoaned consume this wrapper.
//
// The std::function signature mirrors
// ibmw_function_loaned_receiver_callback_ops_t.invoker so dispatcher
// glue can forward 1:1 without an intermediate copy. Named
// LoanedMessageHandlerCallback (not LoanedMessageCallback) to avoid
// collision with the api-layer ibmw::transport::LoanedMessageCallback
// which is a Function<ops> wrapper, not a std::function.
using LoanedMessageHandlerCallback = std::function<void(
    const ibmw_transport_context_base_t* /*ctx_ptr*/,
    const void* /*payload_ptr*/,
    size_t /*payload_size*/,
    std::function<void()>&& /*release*/)>;

struct LoanedMessageHandlerWrapper {
  TransportInfo info;
  LoanedMessageHandlerCallback callback;
};

struct RetainedLoanOptions {
  size_t max_outstanding = 1;
};

class LoanedSubscriptionHandle {
 public:
  virtual ~LoanedSubscriptionHandle() = default;
  virtual void Stop() noexcept = 0;
  virtual bool Close(std::chrono::milliseconds timeout) noexcept = 0;
};

struct SendTypeWrapper {
  TransportInfo info;
  std::unordered_set<std::string> require_cache_serialization_types;

  // Phase 7 §7.2 C2: cached hot-path pointers, populated at
  // RegisterSendType time (dispatcher state == kInit) and read-only
  // consumed in Send. Safe because dispatcher freezes its
  // catalog/filter/driver/seq maps at Start(); driver pointers are
  // copied by value so they survive any subsequent rehash of the
  // dispatcher's per-topic driver map. seq_counter and filter_collector
  // point at addresses inside dispatcher-owned maps that never erase
  // entries after Start().
  std::vector<TransportDriver*> cached_drivers;
#ifdef IBMW_ENABLE_COMM_TRACE
  std::vector<internal::CommTraceSendOverlayV1*> cached_send_overlays;
#endif
  std::atomic<uint64_t>* cached_seq_counter = nullptr;
  const FrameworkAsyncTransportFilterCollector* cached_filter_collector = nullptr;
  ibmw::runtime::core::observability::MetricsEntry* cached_metrics = nullptr;
  std::shared_ptr<ibmw::runtime::core::observability::CachedMetricsGate>
      cached_metrics_gate;
};

class TransportCatalog {
 public:
  TransportCatalog()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~TransportCatalog() = default;

  TransportCatalog(const TransportCatalog&) = delete;
  TransportCatalog& operator=(const TransportCatalog&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  bool OnMessage(std::unique_ptr<MessageHandlerWrapper>&& message_handler_ptr);

  bool ContainsMessageHandler(
      std::string_view msg_type,
      std::string_view topic_name,
      std::string_view pkg_path,
      std::string_view module_name) const;

  bool EraseMessageHandler(
      std::string_view msg_type,
      std::string_view topic_name,
      std::string_view pkg_path,
      std::string_view module_name);

  bool RegisterSendType(
      std::unique_ptr<SendTypeWrapper>&& send_type_wrapper_ptr);

  // Removes a registered publisher from every lookup. The wrapper stays
  // allocated until the catalog is destroyed, because drivers that accepted it
  // may still reference it. Only valid before 'Start'.
  void RetireSendType(const SendTypeWrapper& send_type_wrapper);

  const MessageHandlerWrapper* GetMessageHandlerWrapperPtr(
      std::string_view msg_type,
      std::string_view topic_name,
      std::string_view pkg_path,
      std::string_view module_name) const;

  using ModuleMessageHandlerWrapperMap = std::unordered_map<std::string_view, MessageHandlerWrapper*>;
  const ModuleMessageHandlerWrapperMap* GetModuleMessageHandlerWrapperMapPtr(
      std::string_view msg_type,
      std::string_view topic_name,
      std::string_view pkg_path) const;

  const SendTypeWrapper* GetSendTypeWrapperPtr(
      std::string_view msg_type,
      std::string_view topic_name,
      std::string_view pkg_path,
      std::string_view module_name) const;

  const auto& GetMessageHandlerWrapperMap() const { return message_handler_map_; }
  const auto& GetSendTypeWrapperMap() const { return send_type_wrapper_map_; }

  const auto& GetPubTopicIndexMap() const { return pub_topic_index_map_; }
  const auto& GetSubTopicIndexMap() const { return sub_topic_index_map_; }

 private:
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  struct Key {
    std::string_view msg_type;
    std::string_view topic_name;
    std::string_view pkg_path;
    std::string_view module_name;

    bool operator==(const Key& rhs) const {
      return msg_type == rhs.msg_type &&
             topic_name == rhs.topic_name &&
             pkg_path == rhs.pkg_path &&
             module_name == rhs.module_name;
    }

    struct Hash {
      std::size_t operator()(const Key& k) const {
        return (std::hash<std::string_view>()(k.msg_type)) ^
               (std::hash<std::string_view>()(k.topic_name)) ^
               (std::hash<std::string_view>()(k.pkg_path)) ^
               (std::hash<std::string_view>()(k.module_name));
      }
    };
  };

  struct MTPKey {
    std::string_view msg_type;
    std::string_view topic_name;
    std::string_view pkg_path;

    bool operator==(const MTPKey& rhs) const {
      return msg_type == rhs.msg_type &&
             topic_name == rhs.topic_name &&
             pkg_path == rhs.pkg_path;
    }

    struct Hash {
      std::size_t operator()(const MTPKey& k) const {
        return (std::hash<std::string_view>()(k.msg_type)) ^
               (std::hash<std::string_view>()(k.topic_name)) ^
               (std::hash<std::string_view>()(k.pkg_path));
      }
    };
  };

  std::unordered_map<Key, std::unique_ptr<SendTypeWrapper>, Key::Hash>
      send_type_wrapper_map_;

  // index map, topic:wrapper
  std::unordered_map<std::string_view, std::vector<SendTypeWrapper*>>
      pub_topic_index_map_;

  std::vector<std::unique_ptr<SendTypeWrapper>> retired_send_type_wrappers_;

  std::unordered_map<Key, std::unique_ptr<MessageHandlerWrapper>, Key::Hash>
      message_handler_map_;

  // index map, topic:wrapper
  std::unordered_map<std::string_view, std::vector<MessageHandlerWrapper*>>
      sub_topic_index_map_;

  // index map, msg-topic-pkg:module:wrapper
  std::unordered_map<MTPKey, ModuleMessageHandlerWrapperMap, MTPKey::Hash>
      sub_msg_topic_pkg_index_map_;
};

}  // namespace ibmw::runtime::core::transport
