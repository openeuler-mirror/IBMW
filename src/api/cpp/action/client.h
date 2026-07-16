// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Public ROS 2 Action (DDS-only) client primitives.
//
// ibmw::action::client provides the generic FastDDS plumbing shared by every
// DDS-only action client: a typed service client endpoint (request writer +
// reply reader), a reply listener that filters replies by the canonical
// service identity, a synchronous CallService helper, and endpoint-match
// waiting. The reader GUID accessor reproduces the rmw_fastrtps reply-routing
// convention exactly.
//
// Per the thin-primitives design, this facade owns NO action orchestration:
// the send-goal / get-result / cancel sequencing and any result validation
// stay in user code. The wire-critical helpers (QoS profiles, WriteParams,
// reply matching) are NOT re-exported: every primitive calls the internal
// ros2_action_dds helpers under the hood so the byte-exact wire contract stays
// centralized and internal.

#ifndef IBMW_API_CPP_ACTION_CLIENT_H_
#define IBMW_API_CPP_ACTION_CLIENT_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "internal/ros2_action_dds/fastdds_typesupport.h"
#include "internal/ros2_action_dds/fastdds_wire_contract.h"

namespace ibmw::action::client {

namespace dds = eprosima::fastdds::dds;
namespace rtps = ibmw::internal::ros2_action_dds::rtps_compat;

#if IBMW_ACTION_HAS_MODERN_FASTDDS
inline constexpr auto kReturnOk = eprosima::fastdds::dds::RETCODE_OK;
#else
inline constexpr auto kReturnOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
#endif

template <typename RspT>
struct ReplySlot {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<RspT> response;
};

template <typename RspT>
struct ServiceCallResult {
  bool write_ok = false;
  std::optional<RspT> response;
};

class ScopeExit {
 public:
  explicit ScopeExit(std::function<void()> on_exit) : on_exit_(std::move(on_exit)) {}
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ~ScopeExit() {
    if (on_exit_) {
      on_exit_();
    }
  }

 private:
  std::function<void()> on_exit_;
};

// Reply listener: accepts only the reply whose related sample identity matches
// the originating request (reply_reader_guid + sequence), then publishes it to
// the slot. Non-matching replies are discarded so concurrent in-flight calls
// on shared endpoints never cross-deliver.
template <typename RspT>
class ReplyListener final : public dds::DataReaderListener {
 public:
  ReplyListener(const rtps::GUID_t& reply_reader_guid, uint32_t sequence, ReplySlot<RspT>& slot)
      : reply_reader_guid_(reply_reader_guid), sequence_(sequence), slot_(slot) {}

  void on_data_available(dds::DataReader* reader) override {
    dds::SampleInfo sample_info;
    RspT response;
    while (reader->take_next_sample(&response, &sample_info) == kReturnOk) {
      if (!sample_info.valid_data) {
        continue;
      }
      const auto& related_id = sample_info.related_sample_identity;
      if (!ibmw::internal::ros2_action_dds::ServiceReplyMatchesRequest(related_id, reply_reader_guid_,
                                                                       sequence_)) {
        response = RspT();
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(slot_.mutex);
        slot_.response = response;
      }
      slot_.cv.notify_all();
      response = RspT();
    }
  }

 private:
  rtps::GUID_t reply_reader_guid_;
  uint32_t sequence_;
  ReplySlot<RspT>& slot_;
};

// Reproduces the rmw_fastrtps reply-routing GUID convention: a service client
// stamps requests with its reply reader's GUID so the server echoes it back in
// the related sample identity.
inline rtps::GUID_t ReaderGuid(dds::DataReader* reader) {
  rtps::GUID_t guid = rtps::GUID_t::unknown();
  rtps::iHandle2GUID(guid, reader->get_instance_handle());
  return guid;
}

template <typename ReqT, typename RspT>
class ServiceClientEndpoint {
 public:
  bool Create(dds::DomainParticipant* participant, dds::Publisher* publisher, dds::Subscriber* subscriber,
              const std::string& request_topic_name, const std::string& reply_topic_name) {
    req_ts_ = ibmw::internal::ros2_action_dds::MakeTypeSupport<ReqT>();
    rsp_ts_ = ibmw::internal::ros2_action_dds::MakeTypeSupport<RspT>();
    req_ts_.register_type(participant);
    rsp_ts_.register_type(participant);

    req_topic_ = participant->create_topic(request_topic_name, req_ts_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
    rsp_topic_ = participant->create_topic(reply_topic_name, rsp_ts_.get_type_name(), dds::TOPIC_QOS_DEFAULT);
    if (req_topic_ == nullptr || rsp_topic_ == nullptr) {
      std::cerr << "[FAIL] create service topics failed: " << request_topic_name << std::endl;
      return false;
    }

    dds::DataWriterQos writer_qos;
    dds::DataReaderQos reader_qos;
    ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(writer_qos);
    ibmw::internal::ros2_action_dds::ApplyServiceEndpointQos(reader_qos);

    writer_ = publisher->create_datawriter(req_topic_, writer_qos);
    reader_ = subscriber->create_datareader(rsp_topic_, reader_qos);
    if (writer_ == nullptr || reader_ == nullptr) {
      std::cerr << "[FAIL] create service client writer/reader failed: " << request_topic_name << std::endl;
      return false;
    }
    reply_reader_guid_ = ReaderGuid(reader_);
    return true;
  }

  void AttachListener(std::unique_ptr<ReplyListener<RspT>> listener) {
    DetachListener();
    listener_ = std::move(listener);
    reader_->set_listener(listener_.get());
  }

  void DetachListener() {
    if (reader_ != nullptr) {
      reader_->set_listener(nullptr);
    }
    listener_.reset();
  }

  template <typename ClockT = std::chrono::steady_clock>
  bool WaitForServer(std::chrono::seconds timeout) const {
    const auto deadline = ClockT::now() + timeout;
    while (ClockT::now() <= deadline) {
      dds::PublicationMatchedStatus publication_status;
      dds::SubscriptionMatchedStatus subscription_status;
      const bool publication_ok = writer_->get_publication_matched_status(publication_status) == kReturnOk;
      const bool subscription_ok = reader_->get_subscription_matched_status(subscription_status) == kReturnOk;
      if (publication_ok && subscription_ok && publication_status.current_count > 0 &&
          subscription_status.current_count > 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  bool Send(ReqT& request, uint32_t sequence) {
    auto params = ibmw::internal::ros2_action_dds::MakeServiceRequestWriteParams(reply_reader_guid_, sequence);
    return writer_->write(&request, params);
  }

  const rtps::GUID_t& reply_reader_guid() const { return reply_reader_guid_; }
  dds::DataReader* reader() const { return reader_; }

 private:
  dds::TypeSupport req_ts_;
  dds::TypeSupport rsp_ts_;
  dds::Topic* req_topic_ = nullptr;
  dds::Topic* rsp_topic_ = nullptr;
  dds::DataWriter* writer_ = nullptr;
  dds::DataReader* reader_ = nullptr;
  rtps::GUID_t reply_reader_guid_ = rtps::GUID_t::unknown();
  std::unique_ptr<ReplyListener<RspT>> listener_;
};

template <typename RspT>
std::optional<RspT> WaitForResponse(ReplySlot<RspT>& slot, std::chrono::seconds timeout) {
  std::unique_lock<std::mutex> lock(slot.mutex);
  if (!slot.cv.wait_for(lock, timeout, [&slot] { return slot.response.has_value(); })) {
    return std::nullopt;
  }
  return slot.response;
}

// Synchronous service call: attach a sequence-scoped reply listener, send the
// request stamped with the reply GUID + sequence, and block until the matching
// reply arrives or the timeout elapses. The listener is always detached on
// return.
template <typename ReqT, typename RspT>
ServiceCallResult<RspT> CallService(ServiceClientEndpoint<ReqT, RspT>& endpoint, ReqT& request,
                                    uint32_t sequence, std::chrono::seconds timeout,
                                    const char* write_failure_message) {
  ReplySlot<RspT> slot;
  endpoint.AttachListener(std::make_unique<ReplyListener<RspT>>(endpoint.reply_reader_guid(), sequence, slot));
  ScopeExit listener_guard([&endpoint]() { endpoint.DetachListener(); });
  if (!endpoint.Send(request, sequence)) {
    std::cerr << write_failure_message << std::endl;
    return ServiceCallResult<RspT>{.write_ok = false, .response = std::nullopt};
  }
  return ServiceCallResult<RspT>{.write_ok = true, .response = WaitForResponse(slot, timeout)};
}

}  // namespace ibmw::action::client

#endif  // IBMW_API_CPP_ACTION_CLIENT_H_
