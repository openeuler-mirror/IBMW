// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Native DDS endpoints; independently observe matching by reading real samples.
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "type_support/dds/generic_dds_pubsub_type.h"

namespace {
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
using Message = ibmw::protocols::common::Header;
namespace fd = eprosima::fastdds::dds;
#if IBMW_DDS_HAS_MODERN_FASTDDS
constexpr auto kOk = fd::RETCODE_OK;
#else
const auto kOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
#endif
}

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    const auto domain = static_cast<uint32_t>(std::stoul(argv[1]));
    const std::string policy = argv[2];
    const std::filesystem::path directory = argv[3];
    auto* factory = fd::DomainParticipantFactory::get_instance();
    auto* publisher_participant = factory->create_participant(domain, fd::PARTICIPANT_QOS_DEFAULT);
    auto* subscriber_participant = factory->create_participant(domain, fd::PARTICIPANT_QOS_DEFAULT);
    if (!publisher_participant || !subscriber_participant) throw std::runtime_error("Cannot create DDS participants");
    fd::TypeSupport type(new ibmw::dds::GenericDdsPubSubType<Message>("ibmw::protocols::common::Header"));
    type.register_type(publisher_participant);
    type.register_type(subscriber_participant);
    auto* publisher_topic = publisher_participant->create_topic("/diag/qos", type.get_type_name(), fd::TOPIC_QOS_DEFAULT);
    auto* subscriber_topic = subscriber_participant->create_topic("/diag/qos", type.get_type_name(), fd::TOPIC_QOS_DEFAULT);
    fd::DataWriterQos writer_qos;
    fd::DataReaderQos good_qos, requested_qos;
    writer_qos.reliability().kind = fd::RELIABLE_RELIABILITY_QOS;
    good_qos.reliability().kind = fd::RELIABLE_RELIABILITY_QOS;
    requested_qos.reliability().kind = fd::RELIABLE_RELIABILITY_QOS;
    writer_qos.history().depth = good_qos.history().depth = requested_qos.history().depth = 10;
    if (policy == "deadline") {
      writer_qos.deadline().period = {2, 0};
      good_qos.deadline().period = {2, 0};
      requested_qos.deadline().period = {0, 500000000};
    } else if (policy == "liveliness") {
      requested_qos.liveliness().kind = fd::MANUAL_BY_TOPIC_LIVELINESS_QOS;
    } else if (policy == "liveliness_lease_duration") {
      writer_qos.liveliness().lease_duration = {2, 0};
      writer_qos.liveliness().announcement_period = {0, 500000000};
      good_qos.liveliness().lease_duration = {2, 0};
      requested_qos.liveliness().lease_duration = {1, 0};
    } else if (policy == "ownership") {
      requested_qos.ownership().kind = fd::EXCLUSIVE_OWNERSHIP_QOS;
    } else if (policy == "depth") {
      requested_qos.history().depth = 3;
    }
    fd::PublisherQos publisher_qos;
    fd::SubscriberQos good_subscriber_qos, requested_subscriber_qos;
    if (policy == "partition" || policy == "partition_wildcard") {
      publisher_qos.partition().push_back("robot/front");
      good_subscriber_qos.partition().push_back("robot/front");
      requested_subscriber_qos.partition().push_back(policy == "partition" ? "robot/back" : "robot/*");
    }
    auto* publisher = publisher_participant->create_publisher(publisher_qos);
    auto* good_subscriber = subscriber_participant->create_subscriber(good_subscriber_qos);
    auto* requested_subscriber = subscriber_participant->create_subscriber(requested_subscriber_qos);
    auto* writer = publisher->create_datawriter(publisher_topic, writer_qos);
    auto* good_reader = good_subscriber->create_datareader(subscriber_topic, good_qos);
    auto* requested_reader = requested_subscriber->create_datareader(subscriber_topic, requested_qos);
    if (!writer || !good_reader || !requested_reader) throw std::runtime_error("Cannot create DDS endpoints");
    std::signal(SIGTERM, Stop);
    std::signal(SIGINT, Stop);
    std::ofstream good_log(directory / "good.jsonl"), requested_log(directory / "requested.jsonl"), sent(directory / "sent.jsonl");
    auto drain = [](fd::DataReader* reader, std::ofstream& output) {
      Message value;
      fd::SampleInfo info;
      while (reader->take_next_sample(&value, &info) == kOk)
        if (info.valid_data) output << "{\"sequence\":" << value.sequence_num() << "}" << std::endl;
    };
    std::cout << "READY" << std::endl;
    Message value;
    value.frame_id("qos-acceptance");
    uint32_t sequence = 0;
    while (!stopped) {
      value.sequence_num(++sequence);
      writer->write(&value);
      sent << "{\"sequence\":" << sequence << "}" << std::endl;
      drain(good_reader, good_log);
      drain(requested_reader, requested_log);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    publisher_participant->delete_contained_entities();
    subscriber_participant->delete_contained_entities();
    factory->delete_participant(publisher_participant);
    factory->delete_participant(subscriber_participant);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
