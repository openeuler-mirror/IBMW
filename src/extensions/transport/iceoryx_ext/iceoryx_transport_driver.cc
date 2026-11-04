// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "cpp/util/buffer_array_allocator.h"
#include "iceoryx_ext/global.h"
#include "iceoryx_ext/internal/checked_size.h"
#include "iceoryx_ext/internal/payload_size.h"
#include "iceoryx_ext/internal/reporter_bridge.h"
#ifdef IBMW_ENABLE_REPORTER_V1
#include "iceoryx_ext/internal/reporter_sidecar.h"
#endif
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "iceoryx_posh/mepoo/chunk_header.hpp"
#include <limits>
#include "extension_api/ibmw_reporter_v1.h"
#include <dlfcn.h>

#if defined(__GNUC__) || defined(__clang__)
#define IBMW_NOINLINE __attribute__((noinline))
#else
#define IBMW_NOINLINE
#endif

namespace YAML {
template <>
struct convert<ibmw::extensions::iceoryx_extension::IceoryxTransportDriver::Options> {
  using Options = ibmw::extensions::iceoryx_extension::IceoryxTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["listener_thread_name"] = rhs.listener_thread_name;
    node["listener_thread_sched_policy"] = rhs.listener_thread_sched_policy;
    node["listener_thread_bind_cpu"] = rhs.listener_thread_bind_cpu;
    node["benchmark_profile"] = rhs.benchmark_profile;
    node["subscriber_queue_capacity"] = rhs.subscriber_queue_capacity;
    node["publisher_too_slow_policy"] = rhs.publisher_too_slow_policy;
    node["subscriber_queue_full_policy"] = rhs.subscriber_queue_full_policy;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["listener_thread_name"])
      rhs.listener_thread_name = node["listener_thread_name"].as<std::string>();

    if (node["listener_thread_sched_policy"])
      rhs.listener_thread_sched_policy = node["listener_thread_sched_policy"].as<std::string>();

    if (node["listener_thread_bind_cpu"])
      rhs.listener_thread_bind_cpu = node["listener_thread_bind_cpu"].as<std::vector<uint32_t>>();

    if (node["benchmark_profile"])
      rhs.benchmark_profile = node["benchmark_profile"].as<std::string>();
    if (node["subscriber_queue_capacity"])
      rhs.subscriber_queue_capacity = node["subscriber_queue_capacity"].as<uint64_t>();
    if (node["publisher_too_slow_policy"])
      rhs.publisher_too_slow_policy = node["publisher_too_slow_policy"].as<std::string>();
    if (node["subscriber_queue_full_policy"])
      rhs.subscriber_queue_full_policy = node["subscriber_queue_full_policy"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::iceoryx_extension {
namespace {
constexpr std::string_view kCoreMetricsLosslessProfile =
    "core_metrics_e2e_lossless_v1";
constexpr uint64_t kCoreMetricsLosslessQueueCapacity = 256;

}  // namespace

std::optional<IoxDeliveryOptions>
IceoryxTransportDriver::ResolveDeliveryOptions(const Options& options) noexcept {
  const bool any_explicit = !options.benchmark_profile.empty() ||
                            !options.publisher_too_slow_policy.empty() ||
                            !options.subscriber_queue_full_policy.empty() ||
                            options.subscriber_queue_capacity != 1;
  if (!any_explicit) return IoxDeliveryOptions{};
  if (options.benchmark_profile != kCoreMetricsLosslessProfile ||
      options.publisher_too_slow_policy != "WAIT_FOR_CONSUMER" ||
      options.subscriber_queue_full_policy != "BLOCK_PRODUCER" ||
      options.subscriber_queue_capacity != kCoreMetricsLosslessQueueCapacity) {
    return std::nullopt;
  }
  return IoxDeliveryOptions{.lossless = true,
                            .queue_capacity = kCoreMetricsLosslessQueueCapacity};
}

IceoryxTransportDriver::~IceoryxTransportDriver() {
  DestroyDriverReporterState(reporter_state_);
}

namespace {
const ibmw_reporter_v1* QueryHostReporter() noexcept {
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  if (!host) return nullptr;
  using Query = const ibmw_reporter_v1* (*)(void);
  auto query = reinterpret_cast<Query>(dlsym(host, "ibmw_reporter_query_v1"));
  const ibmw_reporter_v1* api = query ? query() : nullptr;
  dlclose(host);
  return api;
}
}  // namespace
#ifdef IBMW_ENABLE_REPORTER_V1
namespace {
bool ValidReporter(const ibmw_reporter_v1* api) noexcept {
  return api && api->abi_version == IBMW_REPORTER_ABI_VERSION_V1 &&
         api->struct_size >= offsetof(ibmw_reporter_v1, add) + sizeof(api->add) &&
         api->register_slot && api->add;
}
void ReporterAdd(const ibmw_reporter_v1* api, ibmw_reporter_slot_v1* slot,
                  uint64_t value) noexcept {
  if (api && api->add && slot) api->add(api->owner, slot, value);
}
}  // namespace
#endif

extern "C" IBMW_ICEORYX_EXPORT const ibmw_reporter_v1*
ibmw_iceoryx_query_host_reporter_v1(void) noexcept {
  const auto* api = QueryHostReporter();
#ifdef IBMW_ENABLE_REPORTER_V1
  return ValidReporter(api) ? api : nullptr;
#else
  return api;
#endif
}
void IceoryxTransportDriver::Initialize(YAML::Node options_node) {
  static_assert(std::is_nothrow_move_assignable_v<Options>);
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  // shm_init_size is an extension option checked by IceoryxBus::Initialize;
  // a copy in the driver options is a legacy field and ignored.

  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Iceoryx transport driver can only be initialized once.");

  Options options;
  if (options_node && !options_node.IsNull()) {
    MW_REQUIRE(
        !options_node["executor"] && !options_node["callback_executor"] &&
            !options_node["subscribers"],
        "Iceoryx transport driver does not support callback executor options; "
        "use listener_thread_* options or the Context::CreateSubscriber(..., executor, cb) API-layer binding.");
    options = options_node.as<Options>();
  }

  const auto delivery = ResolveDeliveryOptions(options);
  MW_REQUIRE(delivery.has_value(),
             "Iceoryx benchmark delivery controls must be omitted (legacy "
             "defaults) or exactly core_metrics_e2e_lossless_v1 with "
             "WAIT_FOR_CONSUMER, BLOCK_PRODUCER, and queue capacity 256.");
  iceoryx_bus_.SetDeliveryOptions(*delivery);
  options_ = std::move(options);
  reporter_state_ = CreateDriverReporterState();
  if (DriverReporterActive(reporter_state_)) {
    raw_ctx_loan_dispatch_ = &IceoryxTransportDriver::ReportedRawCtxLoan;
    serialized_ctx_loan_dispatch_ =
        &IceoryxTransportDriver::ReportedSerializedCtxLoan;
  } else {
    raw_ctx_loan_dispatch_ = &IceoryxTransportDriver::PlainRawCtxLoan;
    serialized_ctx_loan_dispatch_ =
        &IceoryxTransportDriver::PlainSerializedCtxLoan;
  }
  state_.store(State::kInit);
}

void IceoryxTransportDriver::Start() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  state_.store(State::kStart);
}

bool IceoryxTransportDriver::CanShutdownFromCurrentThread() const noexcept {
  return !IsIceoryxListenerCallbackThread();
}

bool IceoryxTransportDriver::TryPrepareShutdown(
    std::chrono::milliseconds timeout) noexcept {
  if (!CanShutdownFromCurrentThread()) return false;
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    shutdown_started_ = true;
  }
  return iceoryx_bus_.TryPrepareShutdown(timeout);
}

void IceoryxTransportDriver::Shutdown() {
  if (IsIceoryxListenerCallbackThread()) {
    MW_ERROR("IceoryxTransportDriver::Shutdown cannot run on an iceoryx listener callback thread; runtime lifecycle shutdown must run on its owner thread.");
    return;
  }
  if (state_.load() == State::kShutdown) return;
  if (!TryPrepareShutdown(std::chrono::seconds(1))) {
    MW_ERROR("IceoryxTransportDriver::Shutdown could not prepare; resources "
             "remain intact for a retry.");
    return;
  }

  iceoryx_bus_.Shutdown();
  state_.store(State::kShutdown);
}

bool IceoryxTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_) return false;
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");
    namespace util = ibmw::common::util;

    const auto& info = send_type_wrapper.info;
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    bool publisher_created = false;
    auto* publisher =
        iceoryx_bus_.RegisterPublisher(pattern, 0, &publisher_created);
    if (!publisher) return false;
    if (!internal::PreparePublisherRegistration(
            publisher_created,
            [&] {
              return PreparePublisherReporter(reporter_state_, publisher, info,
                                              publisher_created);
            },
            [&] { iceoryx_bus_.RollbackPublisher(pattern, publisher); })) {
      return false;
    }

    MW_INFO("Register publish type to iceoryx transport, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  } catch (...) {
    MW_ERROR("RegisterSendType failed with an unknown exception.");
    return false;
  }
}

bool IceoryxTransportDriver::RegisterLoanedPodTopic(
    const runtime::core::transport::TransportInfo& info,
    size_t initial_size_hint, size_t alignment) noexcept {
  if (state_.load() != State::kInit) {
    MW_ERROR("Loaned POD topics can only be registered in Init state.");
    return false;
  }
  return EnsurePodTopic(info.topic_name, info.msg_type, initial_size_hint,
                        alignment);
}

bool IceoryxTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_) return false;
    MW_REQUIRE(state_.load() == State::kInit,
                             "Method can only be called when state is 'Init'.");
    auto operation = iceoryx_bus_.TryAcquireOperation();
    if (!operation) return false;

    namespace util = ibmw::common::util;

    const auto& info = message_handler.info;
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    auto find_itr = message_handler_map_.find(pattern);
    if (find_itr != message_handler_map_.end()) {
      find_itr->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    // if not registered, register subscriber with url bind to handle to iceoryx
    auto msg_handler_tool =
        std::make_shared<runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool->AddMessageHandlerWrapper(&message_handler);

    auto sched_info_set = std::make_shared<std::atomic_bool>(false);
    auto listener_options = options_;

    auto handle =
        [sched_info_set, listener_options = std::move(listener_options),
         msg_handler_tool](
            const void* payload, size_t payload_size,
            std::function<void()>&& release) {
          const auto validated_pkg_size =
              internal::ValidateEncodedPayloadSize(payload, payload_size);
          if (!validated_pkg_size) {
            MW_WARN("Discard malformed iceoryx transport chunk: size {}.",
                    payload_size);
            if (release) release();
            return;
          }
          try {
            // if not set sched info, set it
            if (!sched_info_set->exchange(true)) [[unlikely]] {
              auto thread_name = listener_options.listener_thread_name;
              if (!thread_name.empty()) {
                runtime::core::util::SetNameForCurrentThread(thread_name);
              }

              auto cpu_set = listener_options.listener_thread_bind_cpu;
              if (!cpu_set.empty()) {
                runtime::core::util::BindCpuForCurrentThread(cpu_set);
              }

              auto sched_policy = listener_options.listener_thread_sched_policy;
              if (!sched_policy.empty()) {
                runtime::core::util::SetCpuSchedForCurrentThread(sched_policy);
              }
            }
            // read data from shared memory : pkg_size | serialization_type | ctx_num | ctx_key1 | ctx_val1 | ... | ctx_keyN | ctx_valN | msg_buffer
            auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

            const uint32_t pkg_size = *validated_pkg_size;

            util::ConstBufferOperator buf_oper(static_cast<const char*>(payload) + 4, pkg_size - 4);

            // get serialization type
            std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
            ctx_ptr->SetSerializationType(serialization_type);

            //  get context meta
            size_t ctx_num = buf_oper.GetUint8();
            for (size_t ii = 0; ii < ctx_num; ++ii) {
              auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
              auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
              ctx_ptr->SetMetaValue(key, val);
            }

            ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, "iceoryx");

            // get msg buffer
            auto remaining_buf = buf_oper.GetRemainingBuffer();

            msg_handler_tool->DoMessageCallback(
                ctx_ptr, serialization_type,
                static_cast<const void*>(remaining_buf.data()),
                remaining_buf.size());
          } catch (const std::exception& e) {
            MW_WARN("Handle Iceoryx transport msg failed, exception info: {}", e.what());
          } catch (...) {
            MW_WARN("Handle Iceoryx transport msg failed with an unknown exception.");
          }
          if (release) release();
        };

    auto registration =
        iceoryx_bus_.RegisterSubscriber(pattern, std::move(handle));
    try {
      const auto emplace_result =
          message_handler_map_.emplace(pattern, std::move(msg_handler_tool));
      if (!emplace_result.second) {
        throw std::runtime_error(
            "Iceoryx message handler URL is already registered");
      }
    } catch (...) {
      registration->Stop();
      throw;
    }

    MW_INFO("Register subscribe type  to iceoryx transport, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

// dynamic allocation for loaned shm:
//
//         .---------------if not enough -----------------.
//         |                                              |
//         v                                              |
// release old shm   ——> loan double size   ——> try to write msg on shm  ——> if enough then publish
//
void IceoryxTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    auto operation = iceoryx_bus_.TryAcquireOperation();
    if (!operation) return;
    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;

    std::string iceoryx_pub_topic = std::string("/transport/") +
                                    util::UrlEncode(info.topic_name) + "/" +
                                    util::UrlEncode(info.msg_type);

    // find publisher
    auto* iox_pub_ctx_ptr =
        iceoryx_bus_.GetPublisher(operation, iceoryx_pub_topic);
    MW_REQUIRE(iox_pub_ctx_ptr != nullptr,
                            "Url: {} not registered for publishing!", iceoryx_pub_topic);

    auto publish_type_support_ref = info.msg_type_support_ref;

    // get serialization type
    std::string_view serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    // statistics context meta
    auto [meta_key_vals_array, meta_key_vals_array_len] = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    MW_REQUIRE(meta_key_vals_array_len / 2 <= 255,
                            "Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    // check serialization cache
    auto& serialization_cache = msg_wrapper.serialization_cache;
    auto finditr = serialization_cache.find(serialization_type);
    if (finditr != serialization_cache.end()) {
      // publish with cache
      auto buffer_array_view_ptr = finditr->second;

      const auto* buffer_array_data = buffer_array_view_ptr->Data();
      const size_t buffer_array_len = buffer_array_view_ptr->Size();
      size_t msg_size = buffer_array_view_ptr->BufferSize();

      size_t shn_size = 4 + 1 + serialization_type.size() + context_meta_kv_size + msg_size;
      auto loaned_shm = iox_pub_ctx_ptr->LoanShm(shn_size);
      util::BufferOperator buf_oper(static_cast<char*>(loaned_shm.Ptr()), loaned_shm.Size());

      buf_oper.SetUint32(shn_size);

      buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

      buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
      for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
        buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
      }

      for (size_t ii = 0; ii < buffer_array_len; ++ii) {
        buf_oper.SetBuffer(
            static_cast<const char*>(buffer_array_data[ii].data),
            buffer_array_data[ii].len);
      }

      iox_pub_ctx_ptr->SendShm(loaned_shm);

      MW_TRACE("Iceoryx publish to '{}'", iceoryx_pub_topic);

      return;
    }

    // publish without cache
    CheckMsg(msg_wrapper);

    size_t min_shm_size = 4 + 1 + serialization_type.size() + context_meta_kv_size;
    auto loaned_shm = iox_pub_ctx_ptr->LoanShm(min_shm_size);

    while (true) {
      util::BufferOperator buf_oper(static_cast<char*>(loaned_shm.Ptr()), loaned_shm.Size());

      // skip pkg_size
      buf_oper.Skip(4);

      // write serialization type on loaned shm
      buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

      // write context meta on loaned shm
      buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
      for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
        buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
      }

      // write msg on loaned shm
      ibmw::util::FlatBufferArrayAllocator allocator(
          static_cast<char*>(loaned_shm.Ptr()) + min_shm_size, loaned_shm.Size() - min_shm_size);
      ibmw::util::BufferArray buffer_array(allocator.NativeHandle());

      bool serialize_ret = info.msg_type_support_ref.Serialize(
          serialization_type,
          msg_wrapper.msg_ptr,
          buffer_array.AllocatorNativeHandle(),
          buffer_array.BufferArrayNativeHandle());

      if (!serialize_ret) [[unlikely]] {
        if (allocator.IsOutOfMemory()) {
          // release old shm and loan a new size shm
          if (loaned_shm.Size() == std::numeric_limits<size_t>::max()) {
            throw std::length_error("Iceoryx shm growth overflow");
          }
          iox_pub_ctx_ptr->UpdateLoanShm(loaned_shm,
                                         loaned_shm.Size() + 1U);
          continue;
        }

        MW_THROW("Serialize failed.");
      }

      // write info pkg length on loaned shm
      buf_oper.JumpTo(0);
      buf_oper.SetUint32(min_shm_size + buffer_array.BufferSize());

      break;
    }

    iox_pub_ctx_ptr->SendShm(loaned_shm);

    MW_TRACE("Iceoryx publish to '{}'", iceoryx_pub_topic);

    return;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

// ====================================================================
// Zero-copy POD path
// ====================================================================
//
// URL convention:
//   normal pub/sub : /transport/<topic>/<msg_type>
//   POD zero-copy  : /transport_zc_pod/<topic>/<msg_type>
//
// Different URL prefix => different iceoryx ServiceDescription, so POD
// subscribers never see standard publishers' framed bytes (which would
// otherwise be misinterpreted as raw payload).

namespace {
inline std::string MakePodKey(std::string_view topic, std::string_view msg_type) {
  std::string k;
  k.reserve(topic.size() + 1 + msg_type.size());
  k.append(topic).append("|").append(msg_type);
  return k;
}

inline std::string MakePodUrl(std::string_view topic, std::string_view msg_type) {
  namespace util = ibmw::common::util;
  return std::string("/transport_zc_pod/") +
         util::UrlEncode(topic) + "/" +
         util::UrlEncode(msg_type);
}

inline std::string MakeStdTransportUrl(std::string_view topic, std::string_view msg_type) {
  namespace util = ibmw::common::util;
  return std::string("/transport/") +
         util::UrlEncode(topic) + "/" +
         util::UrlEncode(msg_type);
}

// The handle mode distinguishes this two-pointer plain token from the
// ops-backed reported token. POD handles store IoxPublisher* directly.
struct PlainCtxMetaLoanToken {
  ibmw::extensions::iceoryx_extension::IoxPublisher* pub;
  void* chunk_ptr;
};
static_assert(sizeof(PlainCtxMetaLoanToken) ==
              sizeof(IoxPublisher*) + sizeof(void*));

struct CtxMetaLoanToken {
  ibmw::extensions::iceoryx_extension::IoxPublisher* pub;
  void* chunk_ptr;     // start of iceoryx user payload (i.e. base of header)
#ifdef IBMW_ENABLE_REPORTER_V1
  const DirectPublisherOps* ops;
#else
  size_t total_size;   // header_size + payload_size, for pkg_size field
#endif
};
static_assert(sizeof(CtxMetaLoanToken) ==
              sizeof(IoxPublisher*) + sizeof(void*) +
#ifdef IBMW_ENABLE_REPORTER_V1
                  sizeof(DirectPublisherOps*)
#else
                  sizeof(size_t)
#endif
              );

class RawLoanGuard {
 public:
  RawLoanGuard(IoxPublisher* publisher, void* chunk) noexcept
      : publisher_(publisher), chunk_(chunk) {}
  ~RawLoanGuard() {
    if (chunk_ != nullptr) publisher_->ReleaseRaw(chunk_);
  }

  RawLoanGuard(const RawLoanGuard&) = delete;
  RawLoanGuard& operator=(const RawLoanGuard&) = delete;

  void Disarm() noexcept { chunk_ = nullptr; }

 private:
  IoxPublisher* publisher_;
  void* chunk_;
};
}  // namespace

bool IceoryxTransportDriver::EnsurePodTopic(std::string_view topic,
                                            std::string_view msg_type,
                                            size_t initial_size_hint,
                                            size_t alignment) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_ || state_.load() != State::kInit)
      return false;
    auto operation = iceoryx_bus_.TryAcquireOperation();
    if (!operation) return false;
    if (initial_size_hint == 0 ||
        initial_size_hint > std::numeric_limits<uint32_t>::max() ||
        alignment == 0 || alignment > std::numeric_limits<uint32_t>::max() ||
        (alignment & (alignment - 1)) != 0) {
      MW_ERROR("EnsurePodTopic: invalid size/alignment ({}, {})",
               initial_size_hint, alignment);
      return false;
    }
    auto key = MakePodKey(topic, msg_type);
    if (const auto existing = pod_pub_map_.find(key);
        existing != pod_pub_map_.end()) {
      return existing->second.initial_size_hint == initial_size_hint &&
             existing->second.alignment == alignment;
    }

    auto url = MakePodUrl(topic, msg_type);
    iceoryx_bus_.RegisterPublisher(url, initial_size_hint);
    auto* pub = iceoryx_bus_.GetPublisher(operation, url);
    if (pub == nullptr) {
      MW_ERROR("EnsurePodTopic: failed to obtain publisher for url={}", url);
      return false;
    }
    pod_pub_map_.emplace(
        std::move(key),
        PodPublisherEndpoint{pub, initial_size_hint, alignment});
    MW_INFO("Registered iceoryx zero-copy POD publisher, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("EnsurePodTopic exception: {}", e.what());
    return false;
  }
}

bool IceoryxTransportDriver::RegisterPodSubscriber(std::string_view topic,
                                                   std::string_view msg_type,
                                                   PodMessageCallback callback) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_ || state_.load() != State::kInit) return false;
    auto url = MakePodUrl(topic, msg_type);

    auto cb_storage =
        std::make_shared<PodMessageCallback>(std::move(callback));

    auto handle = [cb_storage](const void* payload, size_t payload_size,
                               std::function<void()>&& release) {
      try {
        (*cb_storage)(payload, payload_size);
      } catch (const std::exception& e) {
        MW_WARN("POD subscriber callback threw: {}", e.what());
      } catch (...) {
        MW_WARN("POD subscriber callback threw an unknown exception.");
      }
      if (release) release();
    };

    iceoryx_bus_.RegisterSubscriber(url, std::move(handle));
    MW_INFO("Registered iceoryx zero-copy POD subscriber, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("RegisterPodSubscriber exception: {}", e.what());
    return false;
  }
}

// Phase 7 §7.3 §U: receiver-side zero-copy fast lane override.
// Forward to the existing RegisterPodSubscriber() machinery — same POD
// URL ("/transport_zc_pod/<topic>/<msg_type>") so a sender that loaned
// via LoanSample(kPodOnly) and a receiver that attached via this path
// see each other on the SHM channel without any header / ctx-meta in
// between. The dispatcher-supplied LoanedMessageHandlerCallback gets
// wrapped in a callback that drops ctx_ptr (POD path has none) and forwards
// the real release token to the dispatcher.
bool IceoryxTransportDriver::AttachLoaned(
    const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("AttachLoaned called outside Init state.");
    return false;
  }
  if (shutdown_started_) return false;

  try {
    const auto& info = message_handler.info;
    auto loaned_cb = message_handler.callback;
    auto handle = [loaned_cb = std::move(loaned_cb)](
                      const void* payload, size_t size,
                      std::function<void()>&& release) {
      loaned_cb(/*ctx_ptr=*/nullptr, payload, size, std::move(release));
    };
    return static_cast<bool>(iceoryx_bus_.RegisterSubscriber(
        MakePodUrl(info.topic_name, info.msg_type), std::move(handle)));
  } catch (const std::exception& e) {
    MW_ERROR("AttachLoaned exception: {}", e.what());
    return false;
  }
}

namespace {
class IoxLoanedSubscriptionHandle final
    : public runtime::core::transport::LoanedSubscriptionHandle {
 public:
  explicit IoxLoanedSubscriptionHandle(
      std::shared_ptr<IoxSubscriptionState> state)
      : state_(std::move(state)) {}
  ~IoxLoanedSubscriptionHandle() override {
    // Stop is nonblocking. The state remains strongly owned by the bus
    // registry until its worker has detached the listener and observed both
    // callback and retained-loan quiescence; only then does it deregister.
    Stop();
  }

  void Stop() noexcept override { state_->Stop(); }
  bool Close(std::chrono::milliseconds timeout) noexcept override {
    return state_->Close(timeout);
  }

 private:
  std::shared_ptr<IoxSubscriptionState> state_;
};
}  // namespace

std::unique_ptr<runtime::core::transport::LoanedSubscriptionHandle>
IceoryxTransportDriver::AttachLoanedRetained(
    const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler,
    runtime::core::transport::RetainedLoanOptions options) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (state_.load() != State::kInit || options.max_outstanding == 0 ||
      options.max_outstanding >
          iox::MAX_CHUNKS_HELD_PER_SUBSCRIBER_SIMULTANEOUSLY) {
    MW_ERROR("Invalid retained loan max_outstanding {}; iceoryx supports 1..{}.",
             options.max_outstanding,
             iox::MAX_CHUNKS_HELD_PER_SUBSCRIBER_SIMULTANEOUSLY);
    return nullptr;
  }
  if (shutdown_started_) return nullptr;

  try {
    const auto& info = message_handler.info;
    const auto url = MakePodUrl(info.topic_name, info.msg_type);
    if (!iceoryx_bus_.ReserveSubscriberUrl(url)) return nullptr;
    auto callback = message_handler.callback;
    IoxSubscriberRegistrationOptions registration_options{
        .retained = true, .url_reserved = true,
        .max_outstanding = options.max_outstanding};
    if (!PrepareSubscriptionReporter(reporter_state_, registration_options,
                                     info.topic_name, info.msg_type)) {
      iceoryx_bus_.RollbackSubscriberUrl(url);
      return nullptr;
    }
    auto state = iceoryx_bus_.RegisterSubscriber(
        url,
        [callback = std::move(callback)](
            const void* payload, size_t size,
            std::function<void()>&& release) {
          callback(nullptr, payload, size, std::move(release));
        },
        std::move(registration_options));
    if (!CommitSubscriptionReporter(state.get())) {
      // The state owns the transaction. Its stop worker closes and detaches
      // callbacks, waits for callbacks and retained loans, removes the
      // committed URL, and only then destroys (aborts) the transaction.
      iceoryx_bus_.AbortSubscriber(state);
      return nullptr;
    }
    return std::make_unique<IoxLoanedSubscriptionHandle>(std::move(state));
  } catch (const std::exception& e) {
    iceoryx_bus_.RollbackSubscriberUrl(
        MakePodUrl(message_handler.info.topic_name, message_handler.info.msg_type));
    MW_ERROR("AttachLoanedRetained exception: {}", e.what());
    return nullptr;
  } catch (...) {
    iceoryx_bus_.RollbackSubscriberUrl(
        MakePodUrl(message_handler.info.topic_name, message_handler.info.msg_type));
    MW_ERROR("AttachLoanedRetained failed with an unknown exception.");
    return nullptr;
  }
}

#ifdef IBMW_ENABLE_REPORTER_V1
template <bool Reported>
IBMW_NOINLINE bool DirectPublisherOps::RawLoanImpl(
    const DirectPublisherOps* ops, IceoryxTransportDriver* driver,
    IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info,
    size_t requested_size, const ibmw::transport::Context& ctx,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  DirectLoanReporterOutcome<Reported> outcome(
      Reported ? const_cast<DirectReporterBinding*>(&ops->binding) : nullptr);
  std::string_view serialization_type = ctx.GetSerializationType();
  if (serialization_type.empty()) {
    serialization_type = info.msg_type_support_ref.DefaultSerializationType();
  }
  auto kv_map = ctx.GetMetaKeyVals();
  const size_t header_size =
      driver->ComputeCtxMetaHeaderSize(serialization_type, kv_map);
  if (header_size == 0) return false;
  size_t total_size = 0;
  if (!internal::CheckedAddSize(header_size, requested_size, total_size) ||
      total_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  void* chunk = pub->LoanRaw(total_size);
  if (!chunk) return false;
  RawLoanGuard chunk_guard(pub, chunk);
  if (driver->EncodeCtxMetaHeader(
          chunk, total_size, static_cast<uint32_t>(total_size),
          serialization_type, kv_map) != header_size) {
    return false;
  }
  auto* token = new CtxMetaLoanToken{pub, chunk, ops};
  chunk_guard.Disarm();
  out_handle.user_payload_ptr = static_cast<char*>(chunk) + header_size;
  out_handle.user_payload_size = requested_size;
  out_handle.mode = runtime::core::transport::LoanMode::kCtxMetaThenPayload;
  out_handle.driver_token = token;
  out_handle.source_driver = driver;
  if constexpr (Reported) outcome.MarkSuccess();
  return true;
}

bool IceoryxTransportDriver::ReportedRawCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info, size_t requested_size,
    const ibmw::transport::Context& ctx,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  const auto* ops = ReporterSidecarAccess::DirectOps(pub);
  if (ops == nullptr || ops->raw_loan == nullptr) return false;
  return ops->raw_loan(ops, driver, pub, info, requested_size, ctx, out_handle);
}

template bool DirectPublisherOps::RawLoanImpl<true>(
    const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
    const runtime::core::transport::TransportInfo&, size_t,
    const ibmw::transport::Context&,
    runtime::core::transport::LoanedSampleHandle&);
#endif

bool IceoryxTransportDriver::PlainRawCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info, size_t requested_size,
    const ibmw::transport::Context& ctx,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  std::string_view serialization_type = ctx.GetSerializationType();
  if (serialization_type.empty()) {
    serialization_type = info.msg_type_support_ref.DefaultSerializationType();
  }
  auto kv_map = ctx.GetMetaKeyVals();
  const size_t header_size =
      ComputeCtxMetaHeaderSize(serialization_type, kv_map);
  if (header_size == 0) return false;
  size_t total_size = 0;
  if (!internal::CheckedAddSize(header_size, requested_size, total_size) ||
      total_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  void* chunk = pub->LoanRaw(total_size);
  if (chunk == nullptr) return false;
  RawLoanGuard chunk_guard(pub, chunk);
  if (EncodeCtxMetaHeader(chunk, total_size, static_cast<uint32_t>(total_size),
                          serialization_type, kv_map) != header_size) {
    return false;
  }
  auto* token = new PlainCtxMetaLoanToken{pub, chunk};
  chunk_guard.Disarm();
  out_handle.user_payload_ptr = static_cast<char*>(chunk) + header_size;
  out_handle.user_payload_size = requested_size;
  out_handle.mode =
      runtime::core::transport::LoanMode::kCtxMetaThenPayloadUnreported;
  out_handle.driver_token = token;
  out_handle.source_driver = driver;
  return true;
}

#ifndef IBMW_ENABLE_REPORTER_V1
bool IceoryxTransportDriver::ReportedRawCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* publisher,
    const runtime::core::transport::TransportInfo& info, size_t requested_size,
    const ibmw::transport::Context& context,
    runtime::core::transport::LoanedSampleHandle& handle) {
  return PlainRawCtxLoan(driver, publisher, info, requested_size, context,
                         handle);
}
#endif

bool IceoryxTransportDriver::LoanSample(
    const runtime::core::transport::TransportInfo& info,
    size_t requested_size,
    runtime::core::transport::LoanMode mode,
    const ibmw::transport::Context* ctx,
    runtime::core::transport::LoanedSampleHandle& out_handle) noexcept {
  // Phase 7 §X1: ctx-meta path collapsed into the base virtual function.
  if (mode == runtime::core::transport::LoanMode::kCtxMetaThenPayload) {
    if (ctx == nullptr) {
      MW_WARN("LoanSample: kCtxMetaThenPayload requires non-null ctx.");
      return false;
    }
    try {
      auto operation = iceoryx_bus_.TryAcquireOperation();
      if (!operation) return false;
      auto url = MakeStdTransportUrl(info.topic_name, info.msg_type);
      auto* pub = iceoryx_bus_.GetPublisher(operation, url);
      if (pub == nullptr) {
        MW_WARN("LoanSample[ctx-meta]: standard topic not registered. "
                "Call RegisterSendType() for topic={} msg_type={} first.",
                info.topic_name, info.msg_type);
        return false;
      }

      return raw_ctx_loan_dispatch_(this, pub, info, requested_size, *ctx,
                                    out_handle);
    } catch (const std::exception& e) {
      MW_ERROR("LoanSample[ctx-meta] exception: {}", e.what());
      return false;
    }
  }

  // kPodOnly path (ctx ignored).
  try {
    auto operation = iceoryx_bus_.TryAcquireOperation();
    if (!operation) return false;
    auto key = MakePodKey(info.topic_name, info.msg_type);
    auto it = pod_pub_map_.find(key);
    if (it == pod_pub_map_.end()) {
      MW_WARN("LoanSample: POD topic not registered. Call EnsurePodTopic first. "
              "topic={} msg_type={}", info.topic_name, info.msg_type);
      return false;
    }
    void* chunk = it->second.publisher->LoanRaw(requested_size,
                                                it->second.alignment);
    if (chunk == nullptr) return false;

    out_handle.user_payload_ptr = chunk;
    out_handle.user_payload_size = requested_size;
    out_handle.mode = runtime::core::transport::LoanMode::kPodOnly;
    out_handle.driver_token = static_cast<void*>(it->second.publisher);
    out_handle.source_driver = this;  // Phase 7 §7.3 §K-3.3
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("LoanSample exception: {}", e.what());
    return false;
  }
}

// Mirrors the auto-grow loop in Send() but expressed against the public
// LoanRaw/ReleaseRaw API (LoanRaw doesn't have UpdateLoanShm-style in-place
// growth; we must release the old chunk and loan a bigger one). Each retry
// re-encodes the header because the chunk_ptr changes; this is cheap (the
// header is 9 bytes for "cdr"+0 ctx-meta, ~50 B with a couple of KVs) and
// keeps the post-condition simple: chunk is fully populated on success.
//
// `kInitialPayloadHint` is the starting payload region size when no better
// hint is available. The small margin covers ordinary CDR encapsulation,
// alignment, and sequence-length overhead around a 4 KiB value. Larger
// payloads still grow geometrically.
namespace {
constexpr size_t kInitialPayloadHint = 4 * 1024 + 64;
constexpr size_t kMaxLoanRetries = 16;  // 4 KB * 2^16 ~= 256 MB cap

struct PlainSerializedLoanPolicy {
  void Retry() noexcept {}
  void MarkSuccess() noexcept {}
  void* MakeToken(
      IoxPublisher* pub, void* chunk,
      runtime::core::transport::LoanedSampleHandle& out_handle) const {
    out_handle.mode = runtime::core::transport::
        LoanMode::kCtxMetaThenPayloadUnreported;
    return new PlainCtxMetaLoanToken{pub, chunk};
  }
};

#ifdef IBMW_ENABLE_REPORTER_V1
struct ReportedSerializedLoanPolicy {
  ReportedSerializedLoanPolicy(const DirectPublisherOps* direct_ops,
                               DirectReporterBinding* reporter_binding)
      : ops(direct_ops), binding(reporter_binding), outcome(binding) {}

  void Retry() noexcept {
    ReporterAdd(binding->api, binding->slots.retry, 1);
  }
  void MarkSuccess() noexcept { outcome.MarkSuccess(); }
  void* MakeToken(
      IoxPublisher* pub, void* chunk,
      runtime::core::transport::LoanedSampleHandle& out_handle) const {
    out_handle.mode =
        runtime::core::transport::LoanMode::kCtxMetaThenPayload;
    return new CtxMetaLoanToken{pub, chunk, ops};
  }

  const DirectPublisherOps* ops;
  DirectReporterBinding* binding;
  DirectLoanReporterOutcome<true> outcome;
};

struct UnreportedDirectSerializedLoanPolicy {
  explicit UnreportedDirectSerializedLoanPolicy(
      const DirectPublisherOps* direct_ops) noexcept
      : ops(direct_ops) {}

  void Retry() noexcept {}
  void MarkSuccess() noexcept {}
  void* MakeToken(
      IoxPublisher* pub, void* chunk,
      runtime::core::transport::LoanedSampleHandle& out_handle) const {
    out_handle.mode =
        runtime::core::transport::LoanMode::kCtxMetaThenPayload;
    return new CtxMetaLoanToken{pub, chunk, ops};
  }

  const DirectPublisherOps* ops;
};
#endif

template <typename LoanPolicy>
IBMW_NOINLINE bool LoanSerializedSampleImpl(
    IceoryxTransportDriver* driver,
    IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx,
    const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle,
    LoanPolicy& policy) {
    std::string_view serialization_type = ctx.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = info.msg_type_support_ref.DefaultSerializationType();
    }

    auto kv_map = ctx.GetMetaKeyVals();
    size_t header_size = IceoryxTransportDriver::ComputeCtxMetaHeaderSize(
        serialization_type, kv_map);
    if (header_size == 0) return false;

    // Start with header_size + a generous payload hint. On serialize-OOM we
    // double the chunk size and retry. Capped to avoid runaway growth.
    size_t chunk_size = 0;
    if (!internal::CheckedAddSize(header_size, kInitialPayloadHint,
                                  chunk_size) ||
        chunk_size > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    void* chunk = nullptr;
    size_t actual_payload_size = 0;
    bool ok = false;

    for (size_t attempt = 0; attempt < kMaxLoanRetries; ++attempt) {
      chunk = pub->LoanRaw(chunk_size);
      if (chunk == nullptr) {
        // iceoryx pool exhausted -- bail; nothing else to try.
        MW_WARN("LoanSerializedSample: LoanRaw({}) returned null on "
                "attempt {}", chunk_size, attempt);
        return false;
      }

      // Serialize into the post-header region. We do NOT write the header
      // yet because we don't know the final pkg_size (depends on what the
      // serializer produces). Header is written after a successful serialize.
      void* payload_region = static_cast<char*>(chunk) + header_size;
      size_t payload_capacity = chunk_size - header_size;

      ibmw::util::FlatBufferArrayAllocator allocator(payload_region,
                                                    payload_capacity);
      ibmw::util::BufferArray buffer_array(allocator.NativeHandle());

      bool serialize_ret = info.msg_type_support_ref.Serialize(
          serialization_type, msg_ptr,
          buffer_array.AllocatorNativeHandle(),
          buffer_array.BufferArrayNativeHandle());

      if (serialize_ret) {
        actual_payload_size = buffer_array.BufferSize();
        // Now write the header with the final pkg_size.
        size_t total_size = 0;
        if (!internal::CheckedAddSize(header_size, actual_payload_size,
                                      total_size) ||
            total_size > std::numeric_limits<uint32_t>::max()) {
          pub->ReleaseRaw(chunk);
          return false;
        }
        size_t written = IceoryxTransportDriver::EncodeCtxMetaHeader(
            chunk, chunk_size, static_cast<uint32_t>(total_size),
            serialization_type, kv_map);
        if (written != header_size) {
          // Should never happen (we sized chunk_size >= header_size), but
          // be defensive.
          pub->ReleaseRaw(chunk);
          return false;
        }
        ok = true;
        break;
      }

      // Serialize failed. If it was OOM, double and retry; otherwise the
      // serializer is broken for this message -- give up.
      pub->ReleaseRaw(chunk);
      chunk = nullptr;
      if (!allocator.IsOutOfMemory()) {
        MW_ERROR("LoanSerializedSample: serialize failed for non-OOM "
                "reason (topic={} msg_type={})", info.topic_name, info.msg_type);
        return false;
      }
      // A retry means another LoanRaw/Serialize attempt will actually occur;
      // terminal OOM on the final allowed attempt is not a retry.
      if (attempt + 1 >= kMaxLoanRetries ||
          chunk_size > std::numeric_limits<uint32_t>::max() / 2) {
        break;
      }
      chunk_size *= 2;
      policy.Retry();
    }

    if (!ok || chunk == nullptr) {
      MW_ERROR("LoanSerializedSample: gave up after {} growth retries "
              "(final chunk_size={})", kMaxLoanRetries, chunk_size);
      return false;
    }

    RawLoanGuard chunk_guard(pub, chunk);
    void* token = policy.MakeToken(pub, chunk, out_handle);
    chunk_guard.Disarm();
    out_handle.user_payload_ptr =
        static_cast<void*>(static_cast<char*>(chunk) + header_size);
    out_handle.user_payload_size = actual_payload_size;
    out_handle.driver_token = static_cast<void*>(token);
    out_handle.source_driver = driver;  // Phase 7 §7.3 §K-3.3
    policy.MarkSuccess();
    return true;
}

}  // namespace

#ifdef IBMW_ENABLE_REPORTER_V1
template <bool Reported>
IBMW_NOINLINE bool DirectPublisherOps::LoanImpl(
    const DirectPublisherOps* ops, IceoryxTransportDriver* driver,
    IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx, const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  if constexpr (Reported) {
    ReportedSerializedLoanPolicy policy{
        ops, const_cast<DirectReporterBinding*>(&ops->binding)};
    return LoanSerializedSampleImpl(driver, pub, info, ctx, msg_ptr, out_handle,
                                    policy);
  } else {
    UnreportedDirectSerializedLoanPolicy policy{ops};
    return LoanSerializedSampleImpl(driver, pub, info, ctx, msg_ptr, out_handle,
                                    policy);
  }
}

bool IceoryxTransportDriver::ReportedSerializedCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx, const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  const auto* ops = ReporterSidecarAccess::DirectOps(pub);
  if (ops == nullptr || ops->serialized_loan == nullptr) return false;
  return ops->serialized_loan(ops, driver, pub, info, ctx, msg_ptr, out_handle);
}

template <bool Reported, bool Publish>
IBMW_NOINLINE void DirectPublisherOps::CompleteImpl(const DirectPublisherOps* ops,
                                      IoxPublisher* pub,
                                      void* chunk_ptr) noexcept {
  auto completion = [](void* context) noexcept {
    if constexpr (Reported) {
      const auto* direct_ops = static_cast<const DirectPublisherOps*>(context);
      const auto& binding = direct_ops->binding;
      ReporterAdd(binding.api,
                  Publish ? binding.slots.publish : binding.slots.release, 1);
    }
  };
  if (chunk_ptr == nullptr) return;
  pub->loan_tracker_.CompleteTrackedLoan([pub, chunk_ptr, completion, ops] {
    if constexpr (Publish) {
      pub->publisher_.publish(chunk_ptr);
    } else {
      pub->publisher_.release(chunk_ptr);
    }
    completion(const_cast<DirectPublisherOps*>(ops));
  });
}

template bool DirectPublisherOps::LoanImpl<true>(
    const DirectPublisherOps*, IceoryxTransportDriver*, IoxPublisher*,
    const runtime::core::transport::TransportInfo&,
    const ibmw::transport::Context&, const void*,
    runtime::core::transport::LoanedSampleHandle&);
template void DirectPublisherOps::CompleteImpl<true, true>(
    const DirectPublisherOps*, IoxPublisher*, void*) noexcept;
template void DirectPublisherOps::CompleteImpl<true, false>(
    const DirectPublisherOps*, IoxPublisher*, void*) noexcept;

std::unique_ptr<const DirectPublisherOps> MakeUnreportedDirectPublisherOps() {
  return std::make_unique<const DirectPublisherOps>(DirectPublisherOps{
      &DirectPublisherOps::RawLoanImpl<false>,
      &DirectPublisherOps::LoanImpl<false>,
      &DirectPublisherOps::CompleteImpl<false, true>,
      &DirectPublisherOps::CompleteImpl<false, false>, {}});
}

#endif

bool IceoryxTransportDriver::PlainSerializedCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* pub,
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx, const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle) {
  PlainSerializedLoanPolicy policy;
  return LoanSerializedSampleImpl(driver, pub, info, ctx, msg_ptr, out_handle,
                                  policy);
}

#ifndef IBMW_ENABLE_REPORTER_V1
bool IceoryxTransportDriver::ReportedSerializedCtxLoan(
    IceoryxTransportDriver* driver, IoxPublisher* publisher,
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& context, const void* message,
    runtime::core::transport::LoanedSampleHandle& handle) {
  return PlainSerializedCtxLoan(driver, publisher, info, context, message,
                                handle);
}
#endif

#undef IBMW_NOINLINE

bool IceoryxTransportDriver::LoanSerializedSample(
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx,
    const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle) noexcept {
  try {
    auto operation = iceoryx_bus_.TryAcquireOperation();
    if (!operation) return false;
    if (msg_ptr == nullptr) return false;
    if (!info.msg_type_support_ref) {
      MW_WARN("LoanSerializedSample: msg_type_support_ref is empty -- "
              "caller must populate it (see IoxCtxMetaTypedPublisher).");
      return false;
    }

    auto url = MakeStdTransportUrl(info.topic_name, info.msg_type);
    auto* pub = iceoryx_bus_.GetPublisher(operation, url);
    if (pub == nullptr) {
      MW_WARN("LoanSerializedSample: standard topic not registered. "
              "Call RegisterSendType() for topic={} msg_type={} first.",
              info.topic_name, info.msg_type);
      return false;
    }
    return serialized_ctx_loan_dispatch_(this, pub, info, ctx, msg_ptr,
                                         out_handle);
  } catch (const std::exception& e) {
    MW_ERROR("LoanSerializedSample exception: {}", e.what());
    return false;
  }
}

size_t IceoryxTransportDriver::ComputeCtxMetaHeaderSize(
    std::string_view serialization_type,
    const std::unordered_map<std::string_view, std::string_view>& kv) noexcept {
  if (serialization_type.size() > std::numeric_limits<uint8_t>::max())
    return 0;
  if (kv.size() > std::numeric_limits<uint8_t>::max())
    return 0;

  size_t size = 4 + 1 + serialization_type.size() + 1;
  for (const auto& [k, v] : kv) {
    if (k.size() > std::numeric_limits<uint16_t>::max() ||
        v.size() > std::numeric_limits<uint16_t>::max()) {
      return 0;
    }
    size += 2 + k.size() + 2 + v.size();
  }
  return size;
}

size_t IceoryxTransportDriver::EncodeCtxMetaHeader(
    void* buffer, size_t buffer_size,
    uint32_t total_size,
    std::string_view serialization_type,
    const std::unordered_map<std::string_view, std::string_view>& kv) noexcept {
  try {
    namespace util = ibmw::common::util;
    size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv);
    if (header_size == 0) return 0;
    if (buffer_size < header_size) return 0;

    util::BufferOperator buf_oper(static_cast<char*>(buffer), buffer_size);
    buf_oper.SetUint32(total_size);
    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
    buf_oper.SetUint8(static_cast<uint8_t>(kv.size()));
    for (const auto& [k, v] : kv) {
      buf_oper.SetString(k, util::BufferLenType::kUInt16);
      buf_oper.SetString(v, util::BufferLenType::kUInt16);
    }
    return header_size;
  } catch (const std::exception&) {
    return 0;
  }
}

void IceoryxTransportDriver::PublishLoaned(
    runtime::core::transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr)
    return;
  switch (handle.mode) {
    case runtime::core::transport::LoanMode::kCtxMetaThenPayloadUnreported: {
      auto* token = static_cast<PlainCtxMetaLoanToken*>(handle.driver_token);
      token->pub->PublishRaw(token->chunk_ptr);
      delete token;
      break;
    }
#ifdef IBMW_ENABLE_REPORTER_V1
    case runtime::core::transport::LoanMode::kCtxMetaThenPayload: {
      auto* token = static_cast<CtxMetaLoanToken*>(handle.driver_token);
      token->ops->publish(token->ops, token->pub, token->chunk_ptr);
      delete token;
      break;
    }
#endif
    case runtime::core::transport::LoanMode::kPodOnly:
      static_cast<IoxPublisher*>(handle.driver_token)
          ->PublishRaw(handle.user_payload_ptr);
      break;
    default:
      return;
  }
  handle.user_payload_ptr = nullptr;
  handle.driver_token = nullptr;
  handle.user_payload_size = 0;
  handle.source_driver = nullptr;
}

void IceoryxTransportDriver::ReleaseLoaned(
    runtime::core::transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr)
    return;
  switch (handle.mode) {
    case runtime::core::transport::LoanMode::kCtxMetaThenPayloadUnreported: {
      auto* token = static_cast<PlainCtxMetaLoanToken*>(handle.driver_token);
      token->pub->ReleaseRaw(token->chunk_ptr);
      delete token;
      break;
    }
#ifdef IBMW_ENABLE_REPORTER_V1
    case runtime::core::transport::LoanMode::kCtxMetaThenPayload: {
      auto* token = static_cast<CtxMetaLoanToken*>(handle.driver_token);
      token->ops->release(token->ops, token->pub, token->chunk_ptr);
      delete token;
      break;
    }
#endif
    case runtime::core::transport::LoanMode::kPodOnly:
      static_cast<IoxPublisher*>(handle.driver_token)
          ->ReleaseRaw(handle.user_payload_ptr);
      break;
    default:
      return;
  }
  handle.user_payload_ptr = nullptr;
  handle.driver_token = nullptr;
  handle.user_payload_size = 0;
  handle.source_driver = nullptr;
}

}  // namespace ibmw::extensions::iceoryx_extension
