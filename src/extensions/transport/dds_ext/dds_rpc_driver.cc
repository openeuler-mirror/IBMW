// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/dds_rpc_driver.h"

#include <fastdds/dds/subscriber/SampleInfo.hpp>
#if __has_include(<fastdds/dds/core/Time_t.hpp>)
#include <fastdds/dds/core/Time_t.hpp>
#else
#include <fastdds/rtps/common/Time_t.h>
#endif
#if __has_include(<fastdds/rtps/common/InstanceHandle.hpp>)
#include <fastdds/rtps/common/InstanceHandle.hpp>
#else
#include <fastdds/rtps/common/InstanceHandle.h>
#endif
#if __has_include(<fastdds/rtps/common/SampleIdentity.hpp>)
#include <fastdds/rtps/common/SampleIdentity.hpp>
#else
#include <fastdds/rtps/common/SampleIdentity.h>
#endif

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "base/url_parser.h"
#include "yaml-cpp/yaml.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::dds_extension::DdsRpcDriver::Options> {
  using Options = ibmw::extensions::dds_extension::DdsRpcDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;
    node["ros2_compatible"] = rhs.ros2_compatible;
    node["timeout_executor"] = rhs.timeout_executor;
    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["ros2_compatible"])
      rhs.ros2_compatible = node["ros2_compatible"].as<bool>();
    if (node["timeout_executor"])
      rhs.timeout_executor = node["timeout_executor"].as<std::string>();
    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::dds_extension {

namespace {

#if __has_include(<fastdds/dds/core/Time_t.hpp>)
using DdsRpcAckDuration = eprosima::fastdds::dds::Duration_t;
#else
using DdsRpcAckDuration = eprosima::fastrtps::Duration_t;
#endif

/// Encode a service name for ROS2 compatibility.
/// Non-alphanumeric chars (except '/') are replaced with _XX hex encoding.
/// This matches the Ros2NameEncode logic in ros2_extension.
std::string Ros2NameEncode(std::string_view str) {
  static constexpr char kHexUpper[] = "0123456789ABCDEF";
  std::string ret;
  ret.reserve(str.size());
  for (unsigned char c : str) {
    if (std::isalnum(c) || c == '/') {
      ret += static_cast<char>(c);
    } else {
      ret += '_';
      ret += kHexUpper[c >> 4];
      ret += kHexUpper[c & 0xF];
    }
  }
  return ret;
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

DdsRpcDriver::DdsRpcDriver(DdsBus& dds_bus) : dds_bus_(dds_bus) {}

// ============================================================================
// Lifecycle
// ============================================================================

void DdsRpcDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "DDS RPC driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  // Create publisher and subscriber
  auto* participant = dds_bus_.GetParticipant();
  MW_REQUIRE(participant, "DDS participant is null.");

  publisher_ = participant->create_publisher(
      eprosima::fastdds::dds::PUBLISHER_QOS_DEFAULT);
  MW_REQUIRE(publisher_, "Failed to create DDS publisher for RPC.");

  subscriber_ = participant->create_subscriber(
      eprosima::fastdds::dds::SUBSCRIBER_QOS_DEFAULT);
  MW_REQUIRE(subscriber_, "Failed to create DDS subscriber for RPC.");

  // Setup timeout executor
  if (!options_.timeout_executor.empty()) {
    MW_REQUIRE(get_executor_func_,
               "Get executor function is not set before initialize.");
    timeout_executor_ = get_executor_func_(options_.timeout_executor);
    MW_REQUIRE(timeout_executor_,
               "Get timeout executor '{}' failed.", options_.timeout_executor);
    MW_TRACE("DDS RPC driver: timeout executor '{}'.", options_.timeout_executor);
  }

  options_node = options_;

  MW_INFO("DDS RPC driver initialized, ros2_compatible={}.", options_.ros2_compatible);
}

void DdsRpcDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_INFO("DDS RPC driver started ({} servers, {} clients).",
             server_endpoints_.size(), client_endpoints_.size());
}

void DdsRpcDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  auto* participant = dds_bus_.GetParticipant();

  // Clean up client endpoints
  for (auto& [key, ep] : client_endpoints_) {
    if (ep->reader) subscriber_->delete_datareader(ep->reader);
    if (ep->writer) publisher_->delete_datawriter(ep->writer);
  }
  client_listeners_.clear();
  client_endpoints_.clear();

  // Clean up server endpoints
  for (auto& [key, ep] : server_endpoints_) {
    if (ep->reader) subscriber_->delete_datareader(ep->reader);
    if (ep->writer) {
      const auto ret = ep->writer->wait_for_acknowledgments(
          DdsRpcAckDuration{2, 0u});
      if (!DdsRpcWriteSucceeded(ret)) {
        MW_WARN("DDS RPC server '{}' response acknowledgment timed out during shutdown.",
                key);
      }
      publisher_->delete_datawriter(ep->writer);
    }
  }
  server_listeners_.clear();
  server_endpoints_.clear();

  // Clean up topics
  for (auto& [name, topic] : topic_map_) {
    participant->delete_topic(topic);
  }
  topic_map_.clear();

  if (publisher_) {
    participant->delete_publisher(publisher_);
    publisher_ = nullptr;
  }
  if (subscriber_) {
    participant->delete_subscriber(subscriber_);
    subscriber_ = nullptr;
  }

  MW_INFO("DDS RPC driver shutdown.");
}

// ============================================================================
// Topic naming helpers
// ============================================================================

std::string DdsRpcDriver::ExtractServiceName(const std::string& func_name) {
  // func_name may be "ros2:/add_two_ints" or just "/add_two_ints"
  auto pos = func_name.find(':');
  if (pos != std::string::npos) {
    return func_name.substr(pos + 1);
  }
  return func_name;
}

std::string DdsRpcDriver::GetRequestTopicName(const std::string& func_name) const {
  std::string svc = ExtractServiceName(func_name);
  if (options_.ros2_compatible) {
    // ROS2 convention: rq/<service_name>Request
    // Encode non-alphanumeric chars (except '/') for ROS2 compatibility
    svc = Ros2NameEncode(svc);
    if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
    return "rq/" + svc + "Request";
  }
  // Non-ROS2 mode
  if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
  return "ibmw_rpc/rq/" + svc;
}

std::string DdsRpcDriver::GetReplyTopicName(const std::string& func_name) const {
  std::string svc = ExtractServiceName(func_name);
  if (options_.ros2_compatible) {
    svc = Ros2NameEncode(svc);
    if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
    return "rr/" + svc + "Reply";
  }
  if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
  return "ibmw_rpc/rr/" + svc;
}

eprosima::fastdds::dds::Topic* DdsRpcDriver::GetOrCreateTopic(
    const std::string& topic_name,
    eprosima::fastdds::dds::TypeSupport type_support) {
  auto it = topic_map_.find(topic_name);
  if (it != topic_map_.end()) return it->second;

  auto* participant = dds_bus_.GetParticipant();

  // Register type if needed
  type_support.register_type(participant);

  auto* topic = participant->create_topic(
      topic_name, type_support.get_type_name(),
      eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);
  MW_REQUIRE(topic, "Failed to create DDS topic '{}'.", topic_name);

  topic_map_[topic_name] = topic;
  return topic;
}

// ============================================================================
// RegisterServiceFunc
// ============================================================================

bool DdsRpcDriver::RegisterServiceFunc(
    const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Service func can only be registered when state is 'Init'.");
      return false;
    }

    const auto& info = service_func_wrapper.info;
    auto server_key = std::string(info.module_name) + ":" + std::string(info.func_name);

    if (server_endpoints_.count(server_key)) {
      MW_WARN("Service '{}' already registered in DDS RPC driver.", server_key);
      return false;
    }

    auto ep = std::make_unique<ServerEndpoint>();
    ep->func_name = info.func_name;
    ep->service_func_wrapper = &service_func_wrapper;

    // Create topics using native DDS TypeSupport from the service's type supports.
    // This enables wire-compatible interop with ROS2 standard services.
    // If custom_type_support_ptr is set, it points to a C string containing the
    // ROS2 service name (e.g. "/add_two_ints") to use for topic naming instead
    // of the func_name. The name is already in ROS2-ready format, so we
    // construct topics directly (no Ros2NameEncode needed).
    if (info.custom_type_support_ptr) {
      std::string svc(static_cast<const char*>(info.custom_type_support_ptr));
      if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
      ep->dds_topic_rq = "rq/" + svc + "Request";
      ep->dds_topic_rr = "rr/" + svc + "Reply";
    } else {
      ep->dds_topic_rq = GetRequestTopicName(info.func_name);
      ep->dds_topic_rr = GetReplyTopicName(info.func_name);
    }

    auto* req_dds_ts_ptr = static_cast<const eprosima::fastdds::dds::TypeSupport*>(
        info.req_type_support_ref.CustomTypeSupportPtr());
    auto* rsp_dds_ts_ptr = static_cast<const eprosima::fastdds::dds::TypeSupport*>(
        info.rsp_type_support_ref.CustomTypeSupportPtr());
    MW_REQUIRE(req_dds_ts_ptr && *req_dds_ts_ptr,
               "Request DDS TypeSupport is null for service '{}'.", info.func_name);
    MW_REQUIRE(rsp_dds_ts_ptr && *rsp_dds_ts_ptr,
               "Response DDS TypeSupport is null for service '{}'.", info.func_name);

    ep->topic_rq = GetOrCreateTopic(ep->dds_topic_rq, *req_dds_ts_ptr);
    ep->topic_rr = GetOrCreateTopic(ep->dds_topic_rr, *rsp_dds_ts_ptr);

    // QoS: RELIABLE + VOLATILE + KEEP_LAST(10) + data_sharing OFF
    eprosima::fastdds::dds::DataReaderQos rdr_qos;
    rdr_qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
    rdr_qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
    rdr_qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    rdr_qos.history().depth = 10;
    rdr_qos.data_sharing().off();

    eprosima::fastdds::dds::DataWriterQos wrt_qos;
    wrt_qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
    wrt_qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
    wrt_qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    wrt_qos.history().depth = 10;
    wrt_qos.data_sharing().off();

    // Create listener
    auto listener = std::make_unique<ServerListener>(*this, *ep);

    // Create DataReader for requests (with listener)
    ep->reader = subscriber_->create_datareader(ep->topic_rq, rdr_qos, listener.get());
    MW_REQUIRE(ep->reader, "Failed to create DataReader for '{}'.", ep->dds_topic_rq);

    // Create DataWriter for responses
    ep->writer = publisher_->create_datawriter(ep->topic_rr, wrt_qos);
    MW_REQUIRE(ep->writer, "Failed to create DataWriter for '{}'.", ep->dds_topic_rr);

    MW_INFO("DDS RPC server registered: func='{}', rq='{}', rr='{}'",
               info.func_name, ep->dds_topic_rq, ep->dds_topic_rr);

    server_listeners_[server_key] = std::move(listener);
    server_endpoints_[server_key] = std::move(ep);
    return true;

  } catch (const std::exception& e) {
    MW_ERROR("RegisterServiceFunc failed: {}", e.what());
    return false;
  }
}

// ============================================================================
// RegisterClientFunc
// ============================================================================

bool DdsRpcDriver::RegisterClientFunc(
    const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Client func can only be registered when state is 'Init'.");
      return false;
    }

    const auto& info = client_func_wrapper.info;
    auto client_key = std::string(info.module_name) + ":" + std::string(info.func_name);

    if (client_endpoints_.count(client_key)) {
      MW_WARN("Client '{}' already registered in DDS RPC driver.", client_key);
      return false;
    }

    auto ep = std::make_unique<ClientEndpoint>();
    ep->func_name = info.func_name;
    ep->client_func_wrapper = &client_func_wrapper;

    // Create topics using native DDS TypeSupport
    // If custom_type_support_ptr is set, use it as ROS2 service name for topics.
    if (info.custom_type_support_ptr) {
      std::string svc(static_cast<const char*>(info.custom_type_support_ptr));
      if (!svc.empty() && svc[0] == '/') svc = svc.substr(1);
      ep->dds_topic_rq = "rq/" + svc + "Request";
      ep->dds_topic_rr = "rr/" + svc + "Reply";
    } else {
      ep->dds_topic_rq = GetRequestTopicName(info.func_name);
      ep->dds_topic_rr = GetReplyTopicName(info.func_name);
    }

    auto* req_dds_ts_ptr = static_cast<const eprosima::fastdds::dds::TypeSupport*>(
        info.req_type_support_ref.CustomTypeSupportPtr());
    auto* rsp_dds_ts_ptr = static_cast<const eprosima::fastdds::dds::TypeSupport*>(
        info.rsp_type_support_ref.CustomTypeSupportPtr());
    MW_REQUIRE(req_dds_ts_ptr && *req_dds_ts_ptr,
               "Request DDS TypeSupport is null for client '{}'.", info.func_name);
    MW_REQUIRE(rsp_dds_ts_ptr && *rsp_dds_ts_ptr,
               "Response DDS TypeSupport is null for client '{}'.", info.func_name);

    ep->topic_rq = GetOrCreateTopic(ep->dds_topic_rq, *req_dds_ts_ptr);
    ep->topic_rr = GetOrCreateTopic(ep->dds_topic_rr, *rsp_dds_ts_ptr);

    // QoS
    eprosima::fastdds::dds::DataWriterQos wrt_qos;
    wrt_qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
    wrt_qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
    wrt_qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    wrt_qos.history().depth = 10;
    wrt_qos.data_sharing().off();

    eprosima::fastdds::dds::DataReaderQos rdr_qos;
    rdr_qos.reliability().kind = eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
    rdr_qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
    rdr_qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
    rdr_qos.history().depth = 10;
    rdr_qos.data_sharing().off();

    // Create listener
    auto listener = std::make_unique<ClientListener>(*this, *ep);

    // Create DataWriter for requests
    ep->writer = publisher_->create_datawriter(ep->topic_rq, wrt_qos);
    MW_REQUIRE(ep->writer, "Failed to create DataWriter for '{}'.", ep->dds_topic_rq);

    // Create DataReader for replies (with listener)
    ep->reader = subscriber_->create_datareader(ep->topic_rr, rdr_qos, listener.get());
    MW_REQUIRE(ep->reader, "Failed to create DataReader for '{}'.", ep->dds_topic_rr);

    // Get reply DataReader GUID for SampleIdentity
    auto instance_handle = ep->reader->get_instance_handle();
    dds_rpc_rtps::iHandle2GUID(ep->reply_reader_guid, instance_handle);

    // Setup timeout
    if (timeout_executor_) {
      ep->client_tool.RegisterTimeoutExecutor(timeout_executor_);
      ep->client_tool.RegisterTimeoutHandle(
          [](std::shared_ptr<runtime::core::service::ServiceMessageFrame>&& frame) {
            runtime::core::service::CallResultCallback(*frame, ibmw::rpc::Status(IBMW_RPC_STATUS_TIMEOUT));
          });
    }

    MW_INFO("DDS RPC client registered: func='{}', rq='{}', rr='{}'",
               info.func_name, ep->dds_topic_rq, ep->dds_topic_rr);

    client_listeners_[client_key] = std::move(listener);
    client_endpoints_[client_key] = std::move(ep);
    return true;

  } catch (const std::exception& e) {
    MW_ERROR("RegisterClientFunc failed: {}", e.what());
    return false;
  }
}

// ============================================================================
// Call (client side)
// ============================================================================

bool DdsRpcDriver::IsClientReady(std::string_view module, std::string_view function) const noexcept {
  try {
    const auto it = client_endpoints_.find(std::string(module) + ":" + std::string(function));
    if (it == client_endpoints_.end() || state_.load() != State::kStart) return false;
    eprosima::fastdds::dds::PublicationMatchedStatus requests;
    eprosima::fastdds::dds::SubscriptionMatchedStatus responses;
    return it->second->writer->get_publication_matched_status(requests) == kDdsRpcReturnOk &&
           it->second->reader->get_subscription_matched_status(responses) == kDdsRpcReturnOk &&
           requests.current_count > 0 && responses.current_count > 0;
  } catch (...) { return false; }
}

void DdsRpcDriver::Call(
    const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept {
  try {
    if (state_.load() != State::kStart) [[unlikely]] {
      MW_WARN("DDS RPC driver: Call when not in Start state.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    const auto& info = client_call_wrapper_ptr->info;
    auto client_key = std::string(info.module_name) + ":" + std::string(info.func_name);

    auto it = client_endpoints_.find(client_key);
    if (it == client_endpoints_.end()) {
      MW_WARN("Client '{}' not registered in DDS RPC driver.", client_key);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_FUNC_NOT_REGISTERED));
      return;
    }

    auto& ep = *it->second;

    // Generate sequence number
    uint32_t seq = ep.seq_counter.fetch_add(1) + 1;

    // Record for response matching
    auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
    auto record_copy = client_call_wrapper_ptr;
    bool record_ret = ep.client_tool.Record(seq, timeout, std::move(record_copy));
    if (!record_ret) [[unlikely]] {
      MW_ERROR("Failed to record DDS RPC request seq={}.", seq);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    // Send request directly — the native DDS TypeSupport (GenericDdsPubSubType<T>)
    // handles CDR serialization, producing wire-compatible .srv Request payloads.
    dds_rpc_rtps::WriteParams wparams;
    wparams.related_sample_identity().writer_guid() = ep.reply_reader_guid;
    wparams.related_sample_identity().sequence_number(
        dds_rpc_rtps::SequenceNumber_t(0, seq));
    auto write_ret = ep.writer->write(const_cast<void*>(client_call_wrapper_ptr->req_ptr), wparams);
    if (!DdsRpcWriteSucceeded(write_ret)) [[unlikely]] {
      // Remove record
      ep.client_tool.GetRecord(seq);
      MW_WARN("DDS RPC client write failed, func='{}'.", info.func_name);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SEND_REQ_FAILED));
      return;
    }

    MW_TRACE("DDS RPC client sent request: func='{}', seq={}.", info.func_name, seq);

  } catch (const std::exception& e) {
    MW_ERROR("DDS RPC Call exception: {}", e.what());
    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  }
}

// ============================================================================
// ServerListener::on_data_available
// ============================================================================

void DdsRpcDriver::ServerListener::on_data_available(
    eprosima::fastdds::dds::DataReader* reader) {
  const auto* sfw = ep_.service_func_wrapper;
  if (!sfw) {
    MW_ERROR("DDS RPC server: no service func wrapper.");
    return;
  }

  const auto& info = sfw->info;
  eprosima::fastdds::dds::SampleInfo sample_info;

  // Create request object for DDS deserialization
  auto service_req_ptr = info.req_type_support_ref.CreateSharedPtr();

  while (reader->take_next_sample(service_req_ptr.get(), &sample_info) ==
         kDdsRpcReturnOk) {
    if (!sample_info.valid_data) continue;

    // Capture the request identity for response correlation.
    // Identity is transported via RTPS inline QoS (WriteParams/SampleInfo),
    // matching rmw_fastrtps behavior:
    //   - sample_identity = request writer identity (GUID + seq)
    //   - related_sample_identity.writer_guid = client's reply reader GUID
    dds_rpc_rtps::SampleIdentity req_identity = sample_info.sample_identity;
    const auto& related_guid = sample_info.related_sample_identity.writer_guid();
    if (related_guid != dds_rpc_rtps::GUID_t::unknown()) {
      req_identity.writer_guid() = related_guid;
    }

    MW_INFO("DDS RPC server received request: func='{}', identity seq=({},{})",
            info.func_name, req_identity.sequence_number().high, req_identity.sequence_number().low);

    // Create service invoke wrapper
    auto service_invoke_wrapper_ptr = std::make_shared<runtime::core::service::ServiceMessageFrame>(
        runtime::core::service::ServiceMessageFrame{.info = info});

    // Create context
    auto ctx_ptr = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
    service_invoke_wrapper_ptr->ctx_ref = ctx_ptr;
    ctx_ptr->SetFunctionName(info.func_name);
    ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, "dds");

    // Request is already deserialized by DDS GenericDdsPubSubType — no framework deserialization needed
    service_invoke_wrapper_ptr->req_ptr = service_req_ptr.get();

    // Create response object
    auto service_rsp_ptr = info.rsp_type_support_ref.CreateSharedPtr();
    service_invoke_wrapper_ptr->rsp_ptr = service_rsp_ptr.get();

    // Set callback — will be called when service func completes
    auto rid = std::make_shared<dds_rpc_rtps::SampleIdentity>(req_identity);

    service_invoke_wrapper_ptr->callback =
        [this,
         service_invoke_wrapper_ptr,
         ctx_ptr,
         service_req_ptr_captured = service_req_ptr,
         service_rsp_ptr,
         rid](ibmw::rpc::Status status) {
          MW_TRACE("DDS RPC server: service completed, func='{}', status={}.",
                      ep_.func_name, status.Code());

          if (!status.OK()) [[unlikely]] {
            MW_WARN("DDS RPC server: service failed for '{}', status={}. "
                    "Cannot communicate error in native .srv mode.",
                    ep_.func_name, status.Code());
            // In native .srv mode we cannot send error codes — just don't respond.
            // The client will timeout.
            return;
          }

          // Write response directly — GenericDdsPubSubType handles CDR serialization
          dds_rpc_rtps::WriteParams wparams;
          wparams.related_sample_identity(*rid);
          auto write_ok = ep_.writer->write(service_rsp_ptr.get(), wparams);
          if (!DdsRpcWriteSucceeded(write_ok)) [[unlikely]] {
            MW_ERROR("DDS RPC server: failed to write response for '{}'.", ep_.func_name);
          } else {
            MW_TRACE("DDS RPC server sent response: func='{}', identity seq=({},{})",
                     ep_.func_name, rid->sequence_number().high, rid->sequence_number().low);
          }
        };

    // Invoke service
    sfw->service_func(service_invoke_wrapper_ptr);

    // Allocate a fresh request object for the next iteration
    service_req_ptr = info.req_type_support_ref.CreateSharedPtr();
  }
}

// ============================================================================
// ClientListener::on_data_available
// ============================================================================

void DdsRpcDriver::ClientListener::on_data_available(
    eprosima::fastdds::dds::DataReader* reader) {
  const auto& info = ep_.client_func_wrapper->info;
  eprosima::fastdds::dds::SampleInfo sample_info;

  // Create temp response object for DDS deserialization
  auto temp_rsp = info.rsp_type_support_ref.CreateSharedPtr();

  while (reader->take_next_sample(temp_rsp.get(), &sample_info) ==
         kDdsRpcReturnOk) {
    if (!sample_info.valid_data) continue;

    // Extract identity for correlation from RTPS SampleInfo
    // (matches rmw_fastrtps __rmw_take_response: uses related_sample_identity)
    auto& related_id = sample_info.related_sample_identity;

    // Check if this response is for us (match GUID)
    if (related_id.writer_guid() != ep_.reply_reader_guid) {
      continue;
    }
    uint32_t seq = related_id.sequence_number().low;

    MW_TRACE("DDS RPC client received response: func='{}', seq={}.",
                ep_.func_name, seq);

    auto msg_recorder = ep_.client_tool.GetRecord(seq);
    if (!msg_recorder) [[unlikely]] {
      MW_TRACE("DDS RPC client: no record for seq={} (timeout?).", seq);
      // Allocate fresh temp for next iteration
      temp_rsp = info.rsp_type_support_ref.CreateSharedPtr();
      continue;
    }

    auto client_call_wrapper_ptr = std::move(*msg_recorder);

    // Copy response data from temp into the client's pre-allocated rsp_ptr
    info.rsp_type_support_ref.Copy(temp_rsp.get(), client_call_wrapper_ptr->rsp_ptr);

    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_OK));

    // Allocate fresh temp for next iteration
    temp_rsp = info.rsp_type_support_ref.CreateSharedPtr();
  }
}

// ============================================================================
// GenInitializationReport
// ============================================================================

std::list<std::pair<std::string, std::string>> DdsRpcDriver::GenInitializationReport() const noexcept {
  std::vector<std::vector<std::string>> srv_table = {{"func name", "rq topic", "rr topic"}};
  for (const auto& [key, ep] : server_endpoints_) {
    srv_table.push_back({key, ep->dds_topic_rq, ep->dds_topic_rr});
  }

  std::vector<std::vector<std::string>> cli_table = {{"func name", "rq topic", "rr topic"}};
  for (const auto& [key, ep] : client_endpoints_) {
    cli_table.push_back({key, ep->dds_topic_rq, ep->dds_topic_rr});
  }

  return {
      {"DDS RPC Servers", ibmw::common::util::FormatTree(srv_table)},
      {"DDS RPC Clients", ibmw::common::util::FormatTree(cli_table)},
  };
}

}  // namespace ibmw::extensions::dds_extension
