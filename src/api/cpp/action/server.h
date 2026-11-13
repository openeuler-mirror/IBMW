// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) server primitives.
//
// ibmw::action::server provides the generic FastDDS plumbing shared by every
// DDS-only action server: typed service endpoints (request reader + reply
// writer), a request listener that dispatches to an owner method, typed topic
// publishers (feedback / status), and a response writer that stamps the
// canonical ROS 2 service reply identity.
//
// Per the thin-primitives design, this facade owns NO action lifecycle: goal
// acceptance, the execute loop, cancellation, and result/feedback/status
// payload construction stay in user code. The wire-critical helpers
// (QoS profiles, WriteParams, SampleIdentity) are NOT re-exported: every
// primitive calls the internal ros2_action_dds helpers under the hood so the
// byte-exact wire contract stays centralized and internal.

#ifndef IBMW_API_CPP_ACTION_SERVER_H_
#define IBMW_API_CPP_ACTION_SERVER_H_

#include <memory>
#include <string>
#include <vector>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "cpp/action/action_log.h"
#include "internal/ros2_action_dds/fastdds_typesupport.h"
#include "internal/ros2_action_dds/fastdds_wire_contract.h"

namespace ibmw::action::server {

namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::internal::ros2_action_dds::rtps_compat;

#if IBMW_ACTION_HAS_MODERN_FASTDDS
inline constexpr auto kReturnOk = eprosima::fastdds::dds::RETCODE_OK;
#else
inline constexpr auto kReturnOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
#endif

// Request listener: takes every valid request sample, derives the canonical
// service request identity, and dispatches to (owner.*handler)(request,
// identity). OwnerT and the handler signature are supplied by user code so the
// existing member-function dispatch is preserved exactly.
template <typename OwnerT, typename ReqT>
class ServiceListener final : public dds::DataReaderListener {
 public:
  using Handler = void (OwnerT::*)(const ReqT&, const rtps::SampleIdentity&);

  ServiceListener(OwnerT& owner, Handler handler) : owner_(owner), handler_(handler) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo sample_info;
    ReqT request;
    while (reader->take_next_sample(&request, &sample_info) == kReturnOk) {
      if (!sample_info.valid_data) {
        continue;
      }
      const auto identity =
          ibmw::internal::ros2_action_dds::MakeCanonicalServiceRequestIdentity(sample_info);
      (owner_.*handler_)(request, identity);
      request = ReqT();
    }
  }

 private:
  OwnerT& owner_;
  Handler handler_;
};

template <typename OwnerT, typename ReqT, typename RspT>
struct ServiceEndpoint {
  dds::TypeSupport req_ts;
  dds::TypeSupport rsp_ts;
  dds::Topic* req_topic = nullptr;
  dds::Topic* rsp_topic = nullptr;
  dds::DataReader* reader = nullptr;
  dds::DataWriter* writer = nullptr;
  std::unique_ptr<ServiceListener<OwnerT, ReqT>> listener;
};

// Create a service endpoint: register request/reply types, create request and
// reply topics with the service QoS, attach a request listener that dispatches
// to (owner.*handler). Returns false on any creation failure.
template <typename OwnerT, typename ReqT, typename RspT>
bool CreateService(dds::DomainParticipant* participant, dds::Publisher* publisher,
                   dds::Subscriber* subscriber, const std::string& req_topic_name,
                   const std::string& rsp_topic_name,
                   ServiceEndpoint<OwnerT, ReqT, RspT>& endpoint, OwnerT& owner,
                   typename ServiceListener<OwnerT, ReqT>::Handler handler) {
  endpoint.req_ts = ibmw::internal::ros2_action_dds::MakeTypeSupport<ReqT>();
  endpoint.rsp_ts = ibmw::internal::ros2_action_dds::MakeTypeSupport<RspT>();
  endpoint.req_ts.register_type(participant);
  endpoint.rsp_ts.register_type(participant);
  endpoint.req_topic =
      participant->create_topic(req_topic_name, endpoint.req_ts.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  endpoint.rsp_topic =
      participant->create_topic(rsp_topic_name, endpoint.rsp_ts.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  if (endpoint.req_topic == nullptr || endpoint.rsp_topic == nullptr) {
    ActionLog(ActionLogLevel::kError, "create service topics failed: " + req_topic_name);
    return false;
  }
  dds::DataReaderQos reader_qos;
  ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(reader_qos);
  dds::DataWriterQos writer_qos;
  ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(writer_qos);
  endpoint.listener = std::make_unique<ServiceListener<OwnerT, ReqT>>(owner, handler);
  endpoint.reader = subscriber->create_datareader(endpoint.req_topic, reader_qos, endpoint.listener.get());
  endpoint.writer = publisher->create_datawriter(endpoint.rsp_topic, writer_qos);
  if (endpoint.reader == nullptr || endpoint.writer == nullptr) {
    ActionLog(ActionLogLevel::kError, "create service reader/writer failed: " + req_topic_name);
    return false;
  }
  ActionLog(ActionLogLevel::kInfo, "Service endpoint rq=" + req_topic_name + " rr=" + rsp_topic_name);
  return true;
}

// Create a topic publisher (feedback or status). The registered TypeSupport is
// appended to owned_types so the caller keeps it alive for the participant's
// lifetime, matching the existing server ownership model.
template <typename MsgT>
bool CreatePublisher(dds::DomainParticipant* participant, dds::Publisher* publisher,
                     const std::string& topic_name,
                     const ibmw::internal::ros2_action_dds::EndpointQosProfile& qos_profile,
                     std::vector<dds::TypeSupport>& owned_types, dds::Topic*& topic,
                     dds::DataWriter*& writer) {
  auto type_support = ibmw::internal::ros2_action_dds::MakeTypeSupport<MsgT>();
  type_support.register_type(participant);
  owned_types.push_back(type_support);
  topic = participant->create_topic(topic_name, type_support.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  if (topic == nullptr) {
    ActionLog(ActionLogLevel::kError, "create topic failed: " + topic_name);
    return false;
  }
  dds::DataWriterQos qos;
  ibmw::internal::ros2_action_dds::ApplyEndpointQosProfile(qos, qos_profile);
  writer = publisher->create_datawriter(topic, qos);
  if (writer == nullptr) {
    ActionLog(ActionLogLevel::kError, "create writer failed: " + topic_name);
    return false;
  }
  ActionLog(ActionLogLevel::kInfo, "Topic endpoint " + topic_name);
  return true;
}

// Write a service response stamped with the canonical request identity so the
// requester's reply matcher pairs it with the originating request.
template <typename RspT>
void WriteResponse(dds::DataWriter* writer, RspT& response, const rtps::SampleIdentity& identity) {
  auto params = ibmw::internal::ros2_action_dds::MakeServiceResponseWriteParams(identity);
  const auto ret = writer->write(&response, params);
  if (!ret) {
    ActionLog(ActionLogLevel::kWarn, "response write failed");
  }
}

}  // namespace ibmw::action::server

#endif  // IBMW_API_CPP_ACTION_SERVER_H_
