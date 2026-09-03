#include "dds_ext/dds_bus.h"
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/fastdds_compat.h"
#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
#include "dds_ext/internal/dds_comm_trace.h"
#endif
#include "dds_types/util/dds_type_support.h"
#include "engine/transport/transport_catalog.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "engine/transport/transport_framework_async_filter.h"
#include "cpp/transport/transport_handle.h"

#include "DdsGenericMsg.hpp"
#if IBMW_FASTDDS_V3_PLUS
#include "DdsGenericMsgPubSubTypes.hpp"
#else
#include "DdsGenericMsgCdrAux.ipp"
#include "dds_types/util/generic_dds_pubsub_type.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <fastdds/dds/topic/TypeSupport.hpp>
#include <yaml-cpp/yaml.h>

namespace {

using namespace std::chrono_literals;
namespace fastdds = eprosima::fastdds::dds;
namespace dds_ext = ibmw::extensions::dds_extension;
namespace transport = ibmw::runtime::core::transport;

constexpr uint64_t kSampleCount = 1000;
constexpr size_t kPayloadSize = 2 * sizeof(uint64_t);
constexpr char kCaptureCapability[] = "dds.ordinary.nonplain.single_driver.sync_callback";
constexpr char kCaptureContract[] = "mw_perf.p0a_contract.v8";
constexpr char kCaptureContractSha[] = "c8f76997156ecd6e608f0d5d4a0d756d3e5748e8b21c67895936ee004a614575";
#ifdef IBMW_MANAGED_EXACT_FAULT_CANDIDATE
constexpr const char kWrongRunId[] = "runtime-fault-wrong-run-id";
#endif

#ifdef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
struct RollbackHandshake {
  int ready_fd;
  int release_fd;
};

int ParseInheritedFd(const char* name, const char* value) {
  if (*value == '\0') throw std::runtime_error(std::string(name) + " must be a nonnegative FD");
  int fd = -1;
  const char* end = value + std::strlen(value);
  const auto parsed = std::from_chars(value, end, fd);
  if (parsed.ec != std::errc{} || parsed.ptr != end || fd < 0 || fcntl(fd, F_GETFD) == -1) {
    throw std::runtime_error(std::string(name) + " must name an inherited nonnegative FD");
  }
  return fd;
}

std::optional<RollbackHandshake> ParseRollbackHandshake() {
  const char* ready = std::getenv("MW_PERF_ROLLBACK_READY_FD");
  const char* release = std::getenv("MW_PERF_ROLLBACK_RELEASE_FD");
  if (ready == nullptr && release == nullptr) return std::nullopt;
  if (ready == nullptr || release == nullptr) {
    throw std::runtime_error("rollback handshake requires both ready and release FDs");
  }
  return RollbackHandshake{ParseInheritedFd("MW_PERF_ROLLBACK_READY_FD", ready),
                           ParseInheritedFd("MW_PERF_ROLLBACK_RELEASE_FD", release)};
}

void CompleteRollbackHandshake(const RollbackHandshake& handshake) {
  const char ready = 'R';
  ssize_t written;
  do {
    written = write(handshake.ready_fd, &ready, 1);
  } while (written < 0 && errno == EINTR);
  if (written != 1) throw std::runtime_error("rollback ready handshake write failed");

  pollfd descriptor{handshake.release_fd, POLLIN, 0};
  int polled;
  do {
    polled = poll(&descriptor, 1, 30000);
  } while (polled < 0 && errno == EINTR);
  if (polled != 1 || (descriptor.revents & POLLIN) == 0) {
    throw std::runtime_error("rollback release handshake timed out or failed");
  }
  char release = 0;
  ssize_t received;
  do {
    received = read(handshake.release_fd, &release, 1);
  } while (received < 0 && errno == EINTR);
  if (received != 1 || release != 'G') {
    throw std::runtime_error("rollback release handshake expected G");
  }
}
#endif

volatile std::sig_atomic_t stop_requested = 0;

void RequestStop(int) {
  stop_requested = 1;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    if (stop_requested != 0) return false;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

fastdds::TypeSupport MakeTypeSupport() {
#if IBMW_FASTDDS_V3_PLUS
  return fastdds::TypeSupport(new ibmw::dds::GenericMsgPubSubType());
#else
  return fastdds::TypeSupport(new ibmw::dds::GenericDdsPubSubType<
      ibmw::dds::GenericMsg>("ibmw::dds::dds_::GenericMsg_"));
#endif
}

const ibmw_type_support_base_t* GenericMsgTypeSupport() {
  return ibmw::dds::GetDdsMessageTypeSupport<ibmw::dds::GenericMsg>(
      MakeTypeSupport().get_type_name());
}

bool IsPlain(const fastdds::TypeSupport& type) {
#if IBMW_FASTDDS_V3_PLUS
  return type->is_plain(fastdds::DEFAULT_DATA_REPRESENTATION);
#else
  return type->is_plain();
#endif
}

#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
#ifdef IBMW_MANAGED_EXACT_FAULT_CANDIDATE
std::optional<std::string> ParseFault(int argc, char** argv) {
  if (argc != 2) return std::nullopt;
  constexpr char prefix[] = "--fault=";
  const std::string argument(argv[1]);
  if (argument.rfind(prefix, 0) != 0) return std::nullopt;
  const std::string fault = argument.substr(sizeof(prefix) - 1);
  if (fault == "stage_loss" || fault == "callback_loss" ||
      fault == "producer_identity_mismatch" || fault == "stage_status" ||
      fault == "callback_status") return fault;
  return std::nullopt;
}

dds_ext::comm_trace::DeliveryKey FaultDeliveryKey() {
  dds_ext::comm_trace::DeliveryKey key{};
  for (size_t index = 0; index < 16; ++index) {
    key.sample.writer_guid[index] = static_cast<uint8_t>(index + 1);
    key.reader_guid[index] = static_cast<uint8_t>(index + 17);
  }
  key.sample.sequence_high = 0;
  key.sample.sequence_low = static_cast<uint32_t>(kSampleCount + 1);
  return key;
}
#endif

struct CaptureV2Handshake {
  int control_fd = -1;
};
constexpr int kCaptureReleaseWaitMs = 30000;

int ParseCaptureFd(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') throw std::runtime_error(std::string(name) + " is required");
  int fd = -1;
  const auto parsed = std::from_chars(value, value + std::strlen(value), fd);
  if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || fd < 0 || fcntl(fd, F_GETFD) < 0) {
    throw std::runtime_error(std::string(name) + " is not a valid inherited FD");
  }
  return fd;
}

std::optional<CaptureV2Handshake> ParseCaptureV2Handshake() {
  const char* protocol = std::getenv("MW_PERF_CAPTURE_PROTOCOL");
  const char* control = std::getenv("MW_PERF_CAPTURE_CONTROL_FD");
  if (protocol == nullptr && control == nullptr) return std::nullopt;
  if (protocol == nullptr || std::strcmp(protocol, "v2") != 0 || control == nullptr) {
    throw std::runtime_error("v2 capture handshake requires protocol and control FD");
  }
  CaptureV2Handshake handshake{ParseCaptureFd("MW_PERF_CAPTURE_CONTROL_FD")};
  const char* required[] = {"MW_PERF_CAPTURE_ROLE", "MW_PERF_CAPTURE_CAMPAIGN_ID",
                            "MW_PERF_CAPTURE_FAULT", "MW_PERF_CAPTURE_CONTRACT_ID",
                            "MW_PERF_CAPTURE_CONTRACT_SHA256", "MW_PERF_CAPTURE_WORKLOAD_ID",
                            "MW_PERF_CAPABILITY_ID"};
  for (const char* name : required) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0' || std::strlen(value) > 256) {
      throw std::runtime_error(std::string("missing v2 declaration: ") + name);
    }
  }
  if (std::strcmp(std::getenv("MW_PERF_CAPTURE_ROLE"), "exact_anchor") != 0 ||
      std::strcmp(std::getenv("MW_PERF_CAPTURE_FAULT"), "none") != 0 ||
      std::strcmp(std::getenv("MW_PERF_CAPABILITY_ID"), kCaptureCapability) != 0 ||
      std::strcmp(std::getenv("MW_PERF_CAPTURE_WORKLOAD_ID"), kCaptureCapability) != 0 ||
      std::strcmp(std::getenv("MW_PERF_CAPTURE_CONTRACT_ID"), kCaptureContract) != 0 ||
      std::strcmp(std::getenv("MW_PERF_CAPTURE_CONTRACT_SHA256"), kCaptureContractSha) != 0) {
    throw std::runtime_error("unsupported v2 role or constant contract binding mismatch");
  }
  return handshake;
}

void CaptureV2ReadyRelease(const CaptureV2Handshake& handshake) {
  const char ready = 'R';
  if (send(handshake.control_fd, &ready, 1, 0) != 1) throw std::runtime_error("v2 ready write failed");
  pollfd descriptor{handshake.control_fd, POLLIN, 0};
  int result;
  do { result = poll(&descriptor, 1, kCaptureReleaseWaitMs); } while (result < 0 && errno == EINTR);
  if (result != 1 || (descriptor.revents & POLLIN) == 0) throw std::runtime_error("v2 release timeout");
  char release = 0;
  if (recv(handshake.control_fd, &release, 1, 0) != 1 || release != 'G') throw std::runtime_error("v2 release expected G");
}

bool CaptureV2Complete(const CaptureV2Handshake& handshake) {
  const char complete = 'C';
  if (send(handshake.control_fd, &complete, 1, 0) != 1) return false;
  pollfd descriptor{handshake.control_fd, POLLIN | POLLHUP, 0};
  int result;
  do { result = poll(&descriptor, 1, kCaptureReleaseWaitMs); } while (result < 0 && errno == EINTR);
  char evidence = 0;
  ssize_t received = -1;
  if (result == 1 && (descriptor.revents & POLLIN) != 0) {
    do { received = recv(handshake.control_fd, &evidence, 1, 0); } while (received < 0 && errno == EINTR);
  }
  const bool granted = received == 1 && evidence == 'E';
  return close(handshake.control_fd) == 0 && granted;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if !defined(IBMW_TRACE_OFF_BUSINESS_CANDIDATE) && \
    !defined(IBMW_MANAGED_EXACT_FAULT_CANDIDATE)
  const bool exit_after_complete =
      argc == 2 && std::strcmp(argv[1], "--exit-after-complete") == 0;
  const bool verify_overlay_dso =
      argc == 2 && std::strcmp(argv[1], "--verify-overlay-dso") == 0;
  if (argc != 1 && !exit_after_complete && !verify_overlay_dso) {
    std::cerr << "usage: " << argv[0]
              << " [--exit-after-complete|--verify-overlay-dso]" << std::endl;
    return 2;
  }
  if (verify_overlay_dso) {
    setenv("IBMW_COMM_TRACE", "full", 1);
    setenv("IBMW_COMM_TRACE_SINK", "none", 1);
    setenv("MW_PERF_CAPABILITY_ID", dds_ext::comm_trace::kExactCapabilityId, 1);
    setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  }
#endif
#ifdef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << std::endl;
    return 2;
  }
  std::optional<RollbackHandshake> rollback_handshake;
  try {
    rollback_handshake = ParseRollbackHandshake();
  } catch (const std::exception& error) {
    std::cerr << "managed exact candidate startup failure: " << error.what() << std::endl;
    return 1;
  }
#elif defined(IBMW_MANAGED_EXACT_FAULT_CANDIDATE)
  const auto parsed_fault = ParseFault(argc, argv);
  if (!parsed_fault) {
    std::cerr << "usage: " << argv[0]
              << " --fault=stage_loss|callback_loss|producer_identity_mismatch|stage_status|callback_status"
              << std::endl;
    return 2;
  }
  const std::string fault = *parsed_fault;
  if (fault == "producer_identity_mismatch" &&
      setenv("MW_PERF_RUN_ID", kWrongRunId, 1) != 0) {
    std::cerr << "managed exact candidate startup failure: run identity override failed" << std::endl;
    return 1;
  }
#endif
  std::optional<CaptureV2Handshake> capture_v2_handshake;
#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
  try {
    capture_v2_handshake = ParseCaptureV2Handshake();
  } catch (const std::exception& error) {
    std::cerr << "managed exact candidate startup failure: " << error.what() << std::endl;
    return 1;
  }
#endif
  std::signal(SIGTERM, RequestStop);
  std::signal(SIGINT, RequestStop);

#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
  bool trace_initialized = false;
#endif
  bool bus_initialized = false;
  bool dispatcher_initialized = false;
  int result = 1;

  const uint32_t domain_id = 200U + static_cast<uint32_t>(getpid() % 20);
  const std::string unique = std::to_string(getpid()) + "_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string topic = "managed_exact_candidate_" + unique;
  dds_ext::DdsBus bus(domain_id, "ibmw_managed_exact_candidate_" + unique);
  dds_ext::DdsTransportDriver driver(bus);
#if !defined(IBMW_TRACE_OFF_BUSINESS_CANDIDATE) && \
    !defined(IBMW_MANAGED_EXACT_FAULT_CANDIDATE)
  if (verify_overlay_dso) {
    transport::TransportDriver* discovered_driver = &driver;
    auto* overlay = dynamic_cast<transport::internal::CommTraceSendOverlayV1*>(
        discovered_driver);
    if (overlay == nullptr || !overlay->Descriptor().valid()) {
      std::cerr << "managed exact candidate startup failure: extension DSO publish overlay unavailable"
                << std::endl;
      return 1;
    }
  }
#endif
  transport::TransportCatalog catalog;
  transport::FrameworkAsyncTransportFilterBus publish_filters;
  transport::FrameworkAsyncTransportFilterBus subscribe_filters;
  transport::TransportDriverDispatcher dispatcher;
  std::mutex callback_mutex;
  std::condition_variable callback_cv;
  std::atomic<uint32_t> callback_count{0};
  bool callbacks_valid = true;
  bool run_completed = false;
  const uint64_t token = static_cast<uint64_t>(getpid()) ^
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::optional<ibmw::transport::MessageCallback> callback;

  try {
#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
    trace_initialized = dds_ext::comm_trace::Initialize();
    if (!trace_initialized) throw std::runtime_error("comm trace initialization failed");
#endif

    bus.Initialize();
    bus_initialized = true;
    YAML::Node options;
    options["publishers"][0]["topic"] = topic;
    options["subscribers"][0]["topic"] = topic;
    options["subscribers"][0]["loan_mode"] = "sync";
    driver.Initialize(options);

    dispatcher.SetTransportCatalog(&catalog);
    dispatcher.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters);
    dispatcher.SetSubscribeFrameworkAsyncTransportFilterBus(&subscribe_filters);
    dispatcher.SetPubTopicsDriversRules({{".*", {"dds"}}});
    dispatcher.SetSubTopicsDriversRules({{".*", {"dds"}}});
    dispatcher.RegisterTransportDriver(&driver);
    dispatcher.Initialize();
    dispatcher_initialized = true;

    const auto* type_support = GenericMsgTypeSupport();
    if (type_support == nullptr) throw std::runtime_error("GenericMsg type support unavailable");
    const ibmw_string_view_t type_name_view = type_support->type_name(type_support->ctx);
    const std::string msg_type(type_name_view.str, type_name_view.len);
    std::string encoded_type = msg_type;
    std::replace(encoded_type.begin(), encoded_type.end(), ':', '_');
    std::replace(encoded_type.begin(), encoded_type.end(), '.', '_');
    const std::string dds_topic = topic + "_native_" + encoded_type;

    callback.emplace(
        [&](const ibmw_transport_context_base_t*, const void* message,
            ibmw_function_base_t* release) {
          const auto* received = static_cast<const ibmw::dds::GenericMsg*>(message);
          {
            std::lock_guard<std::mutex> lock(callback_mutex);
            const uint32_t arrival = callback_count.load(std::memory_order_relaxed);
            bool valid = received != nullptr && received->serialization_type() == "id" &&
                received->data().size() == kPayloadSize && arrival < kSampleCount;
            uint64_t received_token = 0;
            uint64_t ordinal = 0;
            if (valid) {
              std::memcpy(&received_token, received->data().data(), sizeof(received_token));
              std::memcpy(&ordinal, received->data().data() + sizeof(received_token),
                          sizeof(ordinal));
              valid = received_token == token && ordinal == static_cast<uint64_t>(arrival) + 1;
            }
            callbacks_valid = callbacks_valid && valid;
            callback_count.fetch_add(1, std::memory_order_relaxed);
          }
          ibmw::transport::SubscriberReleaseCallback release_callback(release);
          release_callback();
          callback_cv.notify_one();
        });
    if (!dispatcher.OnMessage(transport::MessageHandlerAccessInfoWrapper{
            .pkg_path = "managed_exact_candidate_pkg",
            .module_name = "managed_exact_candidate_module",
            .topic_name = topic,
            .msg_type_support = type_support,
            .callback = callback->NativeHandle()})) {
      throw std::runtime_error("callback registration failed");
    }
    if (!dispatcher.RegisterSendType(transport::RegisterSendTypeAccessInfoWrapper{
            .pkg_path = "managed_exact_candidate_pkg",
            .module_name = "managed_exact_candidate_module",
            .topic_name = topic,
            .msg_type_support = type_support})) {
      throw std::runtime_error("send type registration failed");
    }
    dispatcher.Start();

    const fastdds::TypeSupport native_type = MakeTypeSupport();
    if (IsPlain(native_type)) throw std::runtime_error("GenericMsg unexpectedly plain");
    if (driver.GetDataWriter(dds_topic) == nullptr) {
      throw std::runtime_error("DDS writer unavailable");
    }
    if (!WaitUntil([&] { return driver.GetPublicationMatchedCount(dds_topic) == 1; }, 5s)) {
      throw std::runtime_error("DDS writer-reader match timed out");
    }
    if (driver.GetPublicationMatchedCount(dds_topic) != 1) {
      throw std::runtime_error("DDS writer must have exactly one matched reader before publish");
    }
    if (capture_v2_handshake) CaptureV2ReadyRelease(*capture_v2_handshake);

    ibmw::dds::GenericMsg sent;
    sent.serialization_type("id");
    sent.msg_context({});
    for (uint64_t ordinal = 1; ordinal <= kSampleCount; ++ordinal) {
      std::vector<uint8_t> payload(kPayloadSize);
      std::memcpy(payload.data(), &token, sizeof(token));
      std::memcpy(payload.data() + sizeof(token), &ordinal, sizeof(ordinal));
      sent.data(std::move(payload));
      dispatcher.Send(transport::SendAccessInfoWrapper{
          .pkg_path = "managed_exact_candidate_pkg",
          .module_name = "managed_exact_candidate_module",
          .topic_name = topic,
          .msg_type = ibmw_string_view_t{msg_type.data(), msg_type.size()},
          .ctx_ptr = nullptr,
          .msg_ptr = &sent});
    }
    if (driver.GetPublicationMatchedCount(dds_topic) != 1) {
      throw std::runtime_error("DDS writer must have exactly one matched reader after publish");
    }

    {
      std::unique_lock<std::mutex> lock(callback_mutex);
      if (!callback_cv.wait_for(lock, 15s, [&] {
            return callback_count.load(std::memory_order_relaxed) >= kSampleCount;
          }) || callback_count.load(std::memory_order_relaxed) != kSampleCount ||
          !callbacks_valid) {
        throw std::runtime_error("DDS callback failed");
      }
    }
#if !defined(IBMW_TRACE_OFF_BUSINESS_CANDIDATE) && \
    !defined(IBMW_MANAGED_EXACT_FAULT_CANDIDATE)
    if (verify_overlay_dso) {
      const auto* stage_query = ibmw_dds_comm_trace_query_v1();
      if (stage_query == nullptr) {
        throw std::runtime_error("extension DSO stage query unavailable");
      }
      std::vector<dds_ext::comm_trace::Event> events(3 * kSampleCount);
      const size_t event_count = stage_query->drain(events.data(), events.size());
      size_t publish_count = 0;
      for (size_t index = 0; index < event_count; ++index) {
        if (events[index].kind == static_cast<uint8_t>(
                dds_ext::comm_trace::EventKind::publish_write)) {
          ++publish_count;
        }
      }
      if (event_count != events.size() || publish_count != kSampleCount) {
        throw std::runtime_error(
            "SendObserved route did not produce the required publish stage events");
      }
    }
#endif
#ifdef IBMW_MANAGED_EXACT_FAULT_CANDIDATE
    const auto* fault_hooks = reinterpret_cast<const dds_ext::comm_trace::RuntimeFaultHooksForTest*>(
        ibmw_dds_comm_trace_query_v2()->reserved);
    if (!fault.empty() && (fault_hooks == nullptr || fault_hooks->abi_version != 1 ||
                           fault_hooks->struct_size != sizeof(*fault_hooks))) {
      throw std::runtime_error("comm trace runtime fault hooks unavailable");
    }
    if (fault == "stage_loss") {
      fault_hooks->inject_stage_dropped();
    } else if (fault == "callback_loss") {
      fault_hooks->inject_exact_dropped();
    } else if (fault == "stage_status") {
      const auto key = FaultDeliveryKey();
      const uint64_t start = fault_hooks->now_ns();
      const uint64_t end = fault_hooks->now_ns();
      fault_hooks->submit(fault_hooks->make_stage_event(
          start, end, key.sample, fault_hooks->topic_hash(topic.data(), topic.size()),
          dds_ext::comm_trace::EventKind::publish_write, 1));
    } else if (fault == "callback_status") {
      const auto key = FaultDeliveryKey();
      const uint64_t start = fault_hooks->now_ns();
      const uint64_t end = fault_hooks->now_ns();
      fault_hooks->exact_submit(start, end, key, 2, 1);
    }
#endif
#ifdef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
    if (rollback_handshake) CompleteRollbackHandshake(*rollback_handshake);
    run_completed = true;
#else
#ifdef IBMW_MANAGED_EXACT_FAULT_CANDIDATE
    constexpr bool exit_after_complete = false;
    constexpr bool verify_overlay_dso = false;
#endif
    if (!exit_after_complete && !verify_overlay_dso) {
      std::cout << "managed exact candidate ready" << std::endl;
      while (stop_requested == 0) std::this_thread::sleep_for(20ms);
    }
    run_completed = true;
#endif
  } catch (const std::exception& error) {
    std::cerr << "managed exact candidate startup failure: " << error.what() << std::endl;
  }

  if (dispatcher_initialized) dispatcher.Shutdown();
  if (bus_initialized) bus.Shutdown();
#ifndef IBMW_TRACE_OFF_BUSINESS_CANDIDATE
  if (trace_initialized) dds_ext::comm_trace::Shutdown();
#endif
  if (capture_v2_handshake && run_completed && !CaptureV2Complete(*capture_v2_handshake)) {
    std::cerr << "managed exact candidate completion attestation failed" << std::endl;
    run_completed = false;
  }
  if (run_completed) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    if (callback_count.load(std::memory_order_relaxed) == kSampleCount && callbacks_valid) {
      result = 0;
    } else {
      std::cerr << "managed exact candidate final callback validation failed" << std::endl;
    }
  }
  return result;
}
