#include "dds_ext/fastdds_compat.h"
#include "dds_ext/internal/dds_comm_trace.h"

#ifdef IBMW_ENABLE_COMM_TRACE
#include "dds_ext/dds_bus.h"
#include "dds_ext/dds_transport_driver.h"
#include "dds_types/util/dds_type_support.h"
#include "engine/transport/transport_catalog.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "engine/transport/transport_framework_async_filter.h"
#include "observability/comm_trace.h"
#include "cpp/transport/transport_handle.h"
#endif

#include "DdsGenericMsg.hpp"
#if IBMW_FASTDDS_V3_PLUS
#include "DdsGenericMsgPubSubTypes.hpp"
#else
#include "DdsGenericMsgCdrAux.ipp"
#include "dds_types/util/generic_dds_pubsub_type.h"
#endif

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <limits.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef IBMW_ENABLE_COMM_TRACE
#include <yaml-cpp/yaml.h>
#endif

#include <fastcdr/config.h>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <gtest/gtest.h>

namespace ibmw::extensions::dds_extension {
namespace {

using namespace std::chrono_literals;
namespace dds = eprosima::fastdds::dds;
namespace compat = fastdds_compat;

class ParticipantOwner final {
 public:
  explicit ParticipantOwner(dds::DomainParticipant* participant)
      : participant_(participant) {}

  ~ParticipantOwner() {
    Shutdown();
  }

  void Shutdown() {
    if (participant_ != nullptr) {
      participant_->delete_contained_entities();
      dds::DomainParticipantFactory::get_instance()->delete_participant(
          participant_);
      participant_ = nullptr;
    }
  }

  ParticipantOwner(const ParticipantOwner&) = delete;
  ParticipantOwner& operator=(const ParticipantOwner&) = delete;

 private:
  dds::DomainParticipant* participant_;
};

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

std::string PrintDladdr(const char* label, void* symbol) {
  Dl_info info{};
  if (symbol != nullptr && dladdr(symbol, &info) != 0 &&
      info.dli_fname != nullptr) {
    std::cout << "provenance " << label << " library=" << info.dli_fname
              << '\n';
    return info.dli_fname;
  } else {
    std::cout << "provenance " << label << " library=<dladdr-failed>\n";
    return {};
  }
}

#ifdef IBMW_EXPECTED_DDS_PREFIX
bool IsUnderExpectedPrefix(const std::string& path) {
  char resolved_path[PATH_MAX]{};
  char resolved_prefix[PATH_MAX]{};
  if (realpath(path.c_str(), resolved_path) == nullptr ||
      realpath(IBMW_EXPECTED_DDS_PREFIX, resolved_prefix) == nullptr) {
    return false;
  }
  const std::string path_with_slash = std::string(resolved_path) + "/";
  const std::string prefix_with_slash = std::string(resolved_prefix) + "/";
  return path_with_slash.rfind(prefix_with_slash, 0) == 0;
}
#endif

bool PrintProvenance() {
#if IBMW_FASTDDS_V3_PLUS
  std::cout << "provenance FastDDS compile=" << FASTDDS_VERSION_STR << '\n';
#else
  std::cout << "provenance FastDDS compile=" << FASTRTPS_VERSION_STR << '\n';
#endif
  std::cout << "provenance FastCDR compile=" << FASTCDR_VERSION_STR << '\n';

  auto* factory_symbol = reinterpret_cast<void*>(
      &dds::DomainParticipantFactory::get_instance);
  const std::string fastdds_library = PrintDladdr("FastDDS", factory_symbol);
  const std::string fastcdr_library = PrintDladdr("FastCDR", dlsym(RTLD_DEFAULT,
      "_ZN8eprosima7fastcdr3Cdr23serialize_encapsulationEv"));
#ifdef IBMW_EXPECTED_DDS_PREFIX
  EXPECT_TRUE(IsUnderExpectedPrefix(fastdds_library))
      << "FastDDS runtime library escaped " << IBMW_EXPECTED_DDS_PREFIX;
  EXPECT_TRUE(IsUnderExpectedPrefix(fastcdr_library))
      << "FastCDR runtime library escaped " << IBMW_EXPECTED_DDS_PREFIX;
#endif
  return !fastdds_library.empty() && !fastcdr_library.empty();
}

bool GuidEqual(const compat::SampleIdentity& lhs,
               const compat::SampleIdentity& rhs) {
  return std::memcmp(lhs.writer_guid().guidPrefix.value,
                     rhs.writer_guid().guidPrefix.value, 12) == 0 &&
      std::memcmp(lhs.writer_guid().entityId.value,
                  rhs.writer_guid().entityId.value, 4) == 0;
}

bool GuidKnown(const compat::SampleIdentity& identity) {
  uint8_t combined = 0;
  for (uint8_t value : identity.writer_guid().guidPrefix.value) combined |= value;
  for (uint8_t value : identity.writer_guid().entityId.value) combined |= value;
  return combined != 0;
}

dds::TypeSupport MakeTypeSupport() {
#if IBMW_FASTDDS_V3_PLUS
  return dds::TypeSupport(new ibmw::dds::GenericMsgPubSubType());
#else
  return dds::TypeSupport(new ibmw::dds::GenericDdsPubSubType<
      ibmw::dds::GenericMsg>("ibmw::dds::dds_::GenericMsg_"));
#endif
}

#ifdef IBMW_ENABLE_COMM_TRACE
const ibmw_type_support_base_t* GenericMsgTypeSupport() {
  return ibmw::dds::GetDdsMessageTypeSupport<ibmw::dds::GenericMsg>(
      MakeTypeSupport().get_type_name());
}

bool GuidBytesEqual(const uint8_t lhs[16], const uint8_t rhs[16]) {
  return std::memcmp(lhs, rhs, 16) == 0;
}

bool SequenceEqual(const comm_trace::Event& event,
                   const comm_trace::SampleKey& key) {
  return event.sequence_high == key.sequence_high &&
         event.sequence_low == key.sequence_low;
}

bool SequenceEqual(const comm_trace::ExactEvent& event,
                   const comm_trace::SampleKey& key) {
  return event.sequence_high == key.sequence_high &&
         event.sequence_low == key.sequence_low;
}

bool IsPlain(const dds::TypeSupport& type) {
#if IBMW_FASTDDS_V3_PLUS
  return type->is_plain(dds::DEFAULT_DATA_REPRESENTATION);
#else
  return type->is_plain();
#endif
}

uint64_t CoreEnvelopeClock() noexcept { return 777; }
#endif

#ifndef IBMW_ENABLE_COMM_TRACE
TEST(DdsNativeIdentityRoundTrip, WriterAssignedIdentityReachesReaderUnchanged) {
  ASSERT_TRUE(PrintProvenance());

  dds::DomainParticipantQos participant_qos;
  participant_qos.name("ibmw_native_identity_roundtrip");
  auto* factory = dds::DomainParticipantFactory::get_instance();
  auto* participant = factory->create_participant(0, participant_qos);
  ASSERT_NE(participant, nullptr);
  ParticipantOwner participant_owner(participant);

  dds::TypeSupport type = MakeTypeSupport();
  ASSERT_EQ(type.register_type(participant), compat::kReturnCodeOk);

  const std::string unique = std::to_string(getpid()) + "_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto* topic = participant->create_topic(
      "ibmw_native_identity_" + unique, type.get_type_name(), dds::TOPIC_QOS_DEFAULT);
  ASSERT_NE(topic, nullptr);
  auto* publisher = participant->create_publisher(dds::PUBLISHER_QOS_DEFAULT);
  auto* subscriber = participant->create_subscriber(dds::SUBSCRIBER_QOS_DEFAULT);
  ASSERT_NE(publisher, nullptr);
  ASSERT_NE(subscriber, nullptr);

  dds::DataWriterQos writer_qos = dds::DATAWRITER_QOS_DEFAULT;
  dds::DataReaderQos reader_qos = dds::DATAREADER_QOS_DEFAULT;
  writer_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
  reader_qos.reliability().kind = dds::RELIABLE_RELIABILITY_QOS;
  auto* writer = publisher->create_datawriter(topic, writer_qos);
  auto* reader = subscriber->create_datareader(topic, reader_qos);
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  ASSERT_TRUE(WaitUntil([&] {
    dds::PublicationMatchedStatus status{};
    return writer->get_publication_matched_status(status) ==
               compat::kReturnCodeOk &&
           status.current_count > 0;
  }, 5s)) << "writer and reader did not match before timeout";

  const uint64_t payload_token =
      static_cast<uint64_t>(std::chrono::steady_clock::now()
                                .time_since_epoch().count()) ^
      static_cast<uint64_t>(getpid());
  std::vector<uint8_t> payload(sizeof(payload_token));
  std::memcpy(payload.data(), &payload_token, sizeof(payload_token));
  ibmw::dds::GenericMsg sent;
  sent.serialization_type("id");
  sent.data(payload);
  sent.msg_context({});

  compat::WriteParams write_params;
  ASSERT_TRUE(compat::WriteWithParams(writer, &sent, write_params));
  const compat::SampleIdentity written_identity =
      compat::WrittenSampleIdentity(write_params);

  ibmw::dds::GenericMsg received;
  dds::SampleInfo sample_info{};
  ASSERT_TRUE(WaitUntil([&] {
    const auto result = reader->take_next_sample(&received, &sample_info);
    return result == compat::kReturnCodeOk && sample_info.valid_data;
  }, 5s)) << "reader did not receive valid data before timeout";

  EXPECT_EQ(received.serialization_type(), sent.serialization_type());
  EXPECT_EQ(received.data(), payload);
  EXPECT_EQ(received.msg_context(), sent.msg_context());

  const compat::SampleIdentity& received_identity =
      compat::ReceivedSampleIdentity(sample_info);
  ASSERT_TRUE(GuidKnown(written_identity));
  ASSERT_FALSE(written_identity.sequence_number().high == -1 &&
               written_identity.sequence_number().low == 0);
  EXPECT_TRUE(GuidEqual(written_identity, received_identity));
  EXPECT_EQ(written_identity.sequence_number().high,
            received_identity.sequence_number().high);
  EXPECT_EQ(written_identity.sequence_number().low,
            received_identity.sequence_number().low);

}
#else
TEST(DdsNativeIdentityRoundTrip,
     OrdinaryNonPlainSingleDriverSyncCallbackPreservesIdentity) {
  ASSERT_TRUE(PrintProvenance());

  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  setenv("MW_PERF_CAPABILITY_ID", comm_trace::kExactCapabilityId, 1);
  setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  comm_trace::ResetForTest();
  ASSERT_TRUE(comm_trace::Initialize());
  comm_trace::SetExactEnabledForTest(true);

  const uint32_t domain_id = 100U + static_cast<uint32_t>(getpid() % 100);
  const std::string unique = std::to_string(getpid()) + "_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string ibmw_topic = "identity_callback_" + unique;
  const auto* type_support = GenericMsgTypeSupport();
  ASSERT_NE(type_support, nullptr);
  const std::string msg_type(
      type_support->type_name(type_support->ctx).str,
      type_support->type_name(type_support->ctx).len);
  std::string encoded_type = msg_type;
  std::replace(encoded_type.begin(), encoded_type.end(), ':', '_');
  std::replace(encoded_type.begin(), encoded_type.end(), '.', '_');
  const std::string dds_topic = ibmw_topic + "_native_" + encoded_type;

  DdsBus bus(domain_id, "ibmw_identity_driver_" + unique);
  bus.Initialize();
  DdsTransportDriver driver(bus);
  YAML::Node options;
  options["subscribers"][0]["topic"] = ibmw_topic;
  options["subscribers"][0]["loan_mode"] = "sync";
  driver.Initialize(options);

  runtime::core::transport::TransportCatalog catalog;
  runtime::core::transport::FrameworkAsyncTransportFilterBus publish_filters;
  runtime::core::transport::FrameworkAsyncTransportFilterBus subscribe_filters;
  runtime::core::transport::TransportDriverDispatcher dispatcher;
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
  dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&subscribe_filters);
  dispatcher.SetPubTopicsDriversRules({{".*", {"dds"}}});
  dispatcher.SetSubTopicsDriversRules({{".*", {"dds"}}});
  dispatcher.RegisterTransportDriver(&driver);
  dispatcher.Initialize();

  std::mutex callback_mutex;
  std::condition_variable callback_cv;
  std::atomic<uint32_t> callback_count{0};
  ibmw::dds::GenericMsg callback_payload;
  ibmw::transport::MessageCallback callback(
      [&](const ibmw_transport_context_base_t*, const void* message,
          ibmw_function_base_t* release) {
        {
          std::lock_guard<std::mutex> lock(callback_mutex);
          callback_payload = *static_cast<const ibmw::dds::GenericMsg*>(message);
          callback_count.fetch_add(1, std::memory_order_relaxed);
        }
        ibmw::transport::SubscriberReleaseCallback release_callback(release);
        release_callback();
        callback_cv.notify_one();
      });
  ASSERT_TRUE(dispatcher.OnMessage(
      runtime::core::transport::MessageHandlerAccessInfoWrapper{
          .pkg_path = "identity_test_pkg",
          .module_name = "identity_test_module",
          .topic_name = ibmw_topic,
          .msg_type_support = type_support,
          .callback = callback.NativeHandle()}));
  const auto* registered_handler = catalog.GetMessageHandlerWrapperPtr(
      msg_type, ibmw_topic, "identity_test_pkg", "identity_test_module");
  ASSERT_NE(registered_handler, nullptr);
  const auto sub_drivers = dispatcher.GetSubTopicDriverInfo();
  const auto sub_driver_it = sub_drivers.find(ibmw_topic);
  ASSERT_NE(sub_driver_it, sub_drivers.end());
  ASSERT_EQ(sub_driver_it->second.size(), 1U);
  EXPECT_EQ(sub_driver_it->second.front(), "dds");
  const uint64_t callback_registration_id =
      registered_handler->callback_registration_id;
  ASSERT_NE(callback_registration_id, 0U);
  ASSERT_TRUE(dispatcher.RegisterSendType(
      runtime::core::transport::RegisterSendTypeAccessInfoWrapper{
          .pkg_path = "identity_test_pkg",
          .module_name = "identity_test_module",
          .topic_name = ibmw_topic,
          .msg_type_support = type_support}));
  dispatcher.Start();

  dds::TypeSupport native_type = MakeTypeSupport();
  ASSERT_FALSE(IsPlain(native_type));
  auto* writer = driver.GetDataWriter(dds_topic);
  ASSERT_NE(writer, nullptr);
  ASSERT_TRUE(WaitUntil(
      [&] { return driver.GetPublicationMatchedCount(dds_topic) == 1; }, 5s))
      << "driver writer did not match the driver reader";

  const uint64_t payload_token =
      static_cast<uint64_t>(std::chrono::steady_clock::now()
                                .time_since_epoch().count()) ^
      static_cast<uint64_t>(getpid());
  std::vector<uint8_t> payload(sizeof(payload_token));
  std::memcpy(payload.data(), &payload_token, sizeof(payload_token));
  ibmw::dds::GenericMsg sent;
  sent.serialization_type("id");
  sent.data(payload);
  sent.msg_context({});

  runtime::core::observability::comm_trace::SetClockForTest(
      &CoreEnvelopeClock);
  runtime::core::observability::comm_trace::SetModeForTest(
      runtime::core::observability::comm_trace::RuntimeMode::full);
  dispatcher.Send(runtime::core::transport::SendAccessInfoWrapper{
      .pkg_path = "identity_test_pkg",
      .module_name = "identity_test_module",
      .topic_name = ibmw_topic,
      .msg_type = ibmw_string_view_t{msg_type.data(), msg_type.size()},
      .ctx_ptr = nullptr,
      .msg_ptr = &sent});

  {
    std::unique_lock<std::mutex> lock(callback_mutex);
    ASSERT_TRUE(callback_cv.wait_for(lock, 5s, [&] {
      return callback_count.load(std::memory_order_relaxed) == 1;
    })) << "driver callback did not complete before timeout";
  }
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(callback_payload.serialization_type(), sent.serialization_type());
  EXPECT_EQ(callback_payload.data(), sent.data());
  EXPECT_EQ(callback_payload.msg_context(), sent.msg_context());

  comm_trace::SetExactEnabledForTest(false);
  const auto* stage_query = ibmw_dds_comm_trace_query_v1();
  ASSERT_NE(stage_query, nullptr);
  std::vector<comm_trace::Event> stage_events(8);
  const size_t stage_count = stage_query->drain(
      stage_events.data(), stage_events.size());
  stage_events.resize(stage_count);
  ASSERT_EQ(stage_events.size(), 3U);
  const auto find_stage = [&](comm_trace::EventKind kind)
      -> const comm_trace::Event* {
    const auto it = std::find_if(stage_events.begin(), stage_events.end(),
        [kind](const comm_trace::Event& event) {
          return event.kind == static_cast<uint8_t>(kind);
        });
    return it == stage_events.end() ? nullptr : &*it;
  };
  const auto* publish_event = find_stage(comm_trace::EventKind::publish_write);
  const auto* take_event = find_stage(comm_trace::EventKind::take_receive);
  const auto* dispatch_event = find_stage(comm_trace::EventKind::dispatch);
  ASSERT_NE(publish_event, nullptr);
  ASSERT_NE(take_event, nullptr);
  ASSERT_NE(dispatch_event, nullptr);
  comm_trace::SampleKey written_key{};
  std::memcpy(written_key.writer_guid, publish_event->writer_guid,
              sizeof(written_key.writer_guid));
  written_key.sequence_high = publish_event->sequence_high;
  written_key.sequence_low = publish_event->sequence_low;
  ASSERT_TRUE(comm_trace::SampleKeyValid(written_key));
  EXPECT_TRUE(comm_trace::WriterMatches(written_key, writer->guid()));
  EXPECT_EQ(publish_event->start_timestamp_ns, 777U);
  for (const auto& event : stage_events) {
    EXPECT_EQ(event.status, 0U);
    EXPECT_EQ(event.schema_version, comm_trace::kSchemaVersion);
    EXPECT_TRUE(GuidBytesEqual(event.writer_guid, written_key.writer_guid));
    EXPECT_TRUE(SequenceEqual(event, written_key));
    EXPECT_LE(event.start_timestamp_ns, event.end_timestamp_ns);
  }
  uint8_t writer_guid[16]{};
  comm_trace::CopyGuid(writer->guid(), writer_guid);
  EXPECT_TRUE(GuidBytesEqual(publish_event->writer_guid, writer_guid));
  EXPECT_FALSE(comm_trace::GuidBytesKnown(publish_event->reader_guid));
  EXPECT_TRUE(comm_trace::GuidBytesKnown(take_event->reader_guid));
  EXPECT_TRUE(GuidBytesEqual(take_event->reader_guid,
                             dispatch_event->reader_guid));
  EXPECT_LE(publish_event->start_timestamp_ns, take_event->start_timestamp_ns);
  EXPECT_LE(take_event->end_timestamp_ns, dispatch_event->start_timestamp_ns);

  const comm_trace::ExactQuery* exact_query = comm_trace::ExactQueryInternal();
  ASSERT_NE(exact_query, nullptr);
  comm_trace::ExactEvent exact_event{};
  ASSERT_EQ(exact_query->drain(&exact_event, 1), 1U);
  EXPECT_EQ(exact_query->drain(&exact_event, 1), 0U);
  EXPECT_EQ(exact_event.kind,
            static_cast<uint8_t>(comm_trace::ExactEventKind::callback_span));
  EXPECT_EQ(exact_event.status, 0U);
  EXPECT_EQ(exact_event.schema_version, comm_trace::kSchemaVersion);
  EXPECT_EQ(exact_event.callback_registration_id, callback_registration_id);
  EXPECT_TRUE(GuidBytesEqual(exact_event.writer_guid, written_key.writer_guid));
  EXPECT_TRUE(SequenceEqual(exact_event, written_key));
  EXPECT_TRUE(GuidBytesEqual(exact_event.reader_guid, take_event->reader_guid));
  EXPECT_LE(dispatch_event->start_timestamp_ns, exact_event.start_timestamp_ns);
  EXPECT_LE(exact_event.start_timestamp_ns, exact_event.end_timestamp_ns);
  EXPECT_LE(exact_event.end_timestamp_ns, dispatch_event->end_timestamp_ns);

  const auto stage_snapshot = stage_query->snapshot();
  EXPECT_EQ(stage_snapshot.pending, 0U);
  EXPECT_EQ(stage_snapshot.dropped, 0U);
  EXPECT_EQ(stage_snapshot.invalid_identity, 0U);
  EXPECT_EQ(stage_snapshot.unsupported_async, 0U);
  const auto exact_snapshot = exact_query->snapshot();
  EXPECT_EQ(exact_snapshot.pending, 0U);
  EXPECT_EQ(exact_snapshot.dropped, 0U);
  EXPECT_EQ(exact_snapshot.invalid_identity, 0U);
  EXPECT_EQ(exact_snapshot.missing_registration, 0U);
  EXPECT_EQ(exact_snapshot.unsupported_async, 0U);
  EXPECT_EQ(exact_snapshot.unsupported_multi_handler, 0U);

  dispatcher.Shutdown();
  bus.Shutdown();
  comm_trace::Shutdown();
}
#endif

}  // namespace
}  // namespace ibmw::extensions::dds_extension
