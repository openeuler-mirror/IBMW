// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "shm_ext/shm_transport_driver.h"

#include <limits>

#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "cpp/util/buffer_array_allocator.h"
#include "shm_ext/global.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::shm_extension::ShmTransportDriver::Options> {
  using Options = ibmw::extensions::shm_extension::ShmTransportDriver::Options;

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
    if (node["listener_thread_name"]) rhs.listener_thread_name = node["listener_thread_name"].as<std::string>();
    if (node["listener_thread_sched_policy"])
      rhs.listener_thread_sched_policy = node["listener_thread_sched_policy"].as<std::string>();
    if (node["listener_thread_bind_cpu"])
      rhs.listener_thread_bind_cpu = node["listener_thread_bind_cpu"].as<std::vector<uint32_t>>();
    if (node["benchmark_profile"]) rhs.benchmark_profile = node["benchmark_profile"].as<std::string>();
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

namespace ibmw::extensions::shm_extension {

namespace transport = runtime::core::transport;

namespace {

// Gives a loan back unless disarmed, so a throwing serializer or allocator
// cannot leak the chunk (and the bus's loan count, which gates shutdown).
template <class ReleaseFn>
class LoanGuard {
 public:
  explicit LoanGuard(ReleaseFn release) noexcept : release_(std::move(release)) {}
  ~LoanGuard() {
    if (armed_) release_();
  }
  LoanGuard(const LoanGuard&) = delete;
  LoanGuard& operator=(const LoanGuard&) = delete;
  void Disarm() noexcept { armed_ = false; }

 private:
  ReleaseFn release_;
  bool armed_ = true;
};

constexpr std::string_view kCoreMetricsLosslessProfile = "core_metrics_e2e_lossless_v1";
constexpr uint64_t kCoreMetricsLosslessQueueCapacity = 256;

// LoanSerializedSample: first payload guess and growth bound (4 KB * 2^16).
constexpr size_t kInitialPayloadHint = 4 * 1024 + 64;
constexpr size_t kMaxLoanRetries = 16;

std::string MakeStdTransportUrl(std::string_view topic, std::string_view msg_type) {
  namespace util = ibmw::common::util;
  return std::string("/transport/") + util::UrlEncode(topic) + "/" + util::UrlEncode(msg_type);
}

std::string MakePodUrl(std::string_view topic, std::string_view msg_type) {
  namespace util = ibmw::common::util;
  return std::string("/transport_zc_pod/") + util::UrlEncode(topic) + "/" + util::UrlEncode(msg_type);
}

std::string MakePodKey(std::string_view topic, std::string_view msg_type) {
  std::string k;
  k.reserve(topic.size() + 1 + msg_type.size());
  k.append(topic).append("|").append(msg_type);
  return k;
}

bool CheckedAddSize(size_t lhs, size_t rhs, size_t& result) noexcept {
  if (rhs > std::numeric_limits<size_t>::max() - lhs) return false;
  result = lhs + rhs;
  return true;
}

// pkg_size prefix of a received frame: at least itself, at most the chunk.
std::optional<uint32_t> ValidateEncodedPayloadSize(const void* payload, size_t payload_size) noexcept {
  if (payload == nullptr || payload_size < sizeof(uint32_t)) return std::nullopt;
  const auto encoded = ibmw::common::util::GetUint32FromBuf(static_cast<const char*>(payload));
  if (encoded < sizeof(uint32_t) || encoded > payload_size) return std::nullopt;
  return encoded;
}

std::string_view SerializationTypeOf(const ibmw::transport::Context& ctx, const transport::TransportInfo& info) {
  std::string_view serialization_type = ctx.GetSerializationType();
  if (serialization_type.empty()) serialization_type = info.msg_type_support_ref.DefaultSerializationType();
  return serialization_type;
}

void ClearHandle(transport::LoanedSampleHandle& handle) noexcept {
  handle.user_payload_ptr = nullptr;
  handle.driver_token = nullptr;
  handle.user_payload_size = 0;
  handle.source_driver = nullptr;
}

class ShmLoanedSubscriptionHandle final : public transport::LoanedSubscriptionHandle {
 public:
  explicit ShmLoanedSubscriptionHandle(std::shared_ptr<ShmSubscription> subscription)
      : subscription_(std::move(subscription)) {}
  ~ShmLoanedSubscriptionHandle() override { Stop(); }

  void Stop() noexcept override { subscription_->Stop(); }
  bool Close(std::chrono::milliseconds timeout) noexcept override { return subscription_->Close(timeout); }

 private:
  std::shared_ptr<ShmSubscription> subscription_;
};

}  // namespace

std::optional<ShmDeliveryOptions> ShmTransportDriver::ResolveDeliveryOptions(const Options& options) noexcept {
  const bool any_explicit = !options.benchmark_profile.empty() || !options.publisher_too_slow_policy.empty() ||
                            !options.subscriber_queue_full_policy.empty() || options.subscriber_queue_capacity != 1;
  if (!any_explicit) return ShmDeliveryOptions{};
  if (options.benchmark_profile != kCoreMetricsLosslessProfile ||
      options.publisher_too_slow_policy != "WAIT_FOR_CONSUMER" ||
      options.subscriber_queue_full_policy != "BLOCK_PRODUCER" ||
      options.subscriber_queue_capacity != kCoreMetricsLosslessQueueCapacity) {
    return std::nullopt;
  }
  ShmDeliveryOptions delivery;
  delivery.lossless = true;
  delivery.queue_capacity = static_cast<uint32_t>(kCoreMetricsLosslessQueueCapacity);
  return delivery;
}

void ShmTransportDriver::Initialize(YAML::Node options_node) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  MW_REQUIRE(state_.load() == State::kPreInit, "Shm transport driver can only be initialized once.");

  Options options;
  if (options_node && !options_node.IsNull()) {
    MW_REQUIRE(!options_node["executor"] && !options_node["callback_executor"] && !options_node["subscribers"],
               "Shm transport driver does not support callback executor options; "
               "use listener_thread_* options or the Context::CreateSubscriber(..., executor, cb) API-layer binding.");
    options = options_node.as<Options>();
  }

  const auto delivery = ResolveDeliveryOptions(options);
  MW_REQUIRE(delivery.has_value(),
             "Shm benchmark delivery controls must be omitted (legacy defaults) or exactly "
             "core_metrics_e2e_lossless_v1 with WAIT_FOR_CONSUMER, BLOCK_PRODUCER, and queue capacity 256.");
  bus_.SetDeliveryOptions(*delivery);
  if (!options.listener_thread_name.empty()) bus_.SetListenerThreadName(options.listener_thread_name);
  options_ = std::move(options);
  state_.store(State::kInit);
}

void ShmTransportDriver::Start() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  MW_REQUIRE(state_.load() == State::kInit, "Method can only be called when state is 'Init'.");
  state_.store(State::kStart);
}

bool ShmTransportDriver::CanShutdownFromCurrentThread() const noexcept { return !IsShmCallbackThread(); }

bool ShmTransportDriver::TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept {
  if (!CanShutdownFromCurrentThread()) return false;
  {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    shutdown_started_ = true;
  }
  return bus_.TryPrepareShutdown(timeout);
}

void ShmTransportDriver::Shutdown() {
  if (IsShmCallbackThread()) {
    MW_ERROR("ShmTransportDriver::Shutdown cannot run on a shm listener callback thread; runtime lifecycle "
             "shutdown must run on its owner thread.");
    return;
  }
  if (state_.load() == State::kShutdown) return;
  if (!TryPrepareShutdown(std::chrono::seconds(1))) {
    MW_ERROR("ShmTransportDriver::Shutdown could not prepare; resources remain intact for a retry.");
    return;
  }
  bus_.Shutdown();
  state_.store(State::kShutdown);
}

std::list<std::pair<std::string, std::string>> ShmTransportDriver::GenInitializationReport() const noexcept {
  try {
    const auto delivery = bus_.DeliveryOptions();
    const auto& bus_options = bus_.Options();
    return {
        {"driver", name_},
        {"implementation", "ibmw selfdev shm (daemonless, memfd + futex)"},
        {"domain", bus_options.domain.empty() ? std::string("default") : bus_options.domain},
        // Effective location after IBMW_SHM_ROOT / IBMW_SHM_DOMAIN overrides.
        {"service directory", shm::Runtime::ServiceDirectory()},
        {"delivery", delivery.lossless ? std::string("lossless") : std::string("latest")},
        {"subscriber queue capacity", std::to_string(delivery.queue_capacity)},
        {"publishers", std::to_string(bus_.PublisherCount())},
        {"subscriptions", std::to_string(bus_.SubscriptionCount())},
        {"listener threads", std::to_string(bus_.ListenerThreads())},
    };
  } catch (...) {
    return {};
  }
}

bool ShmTransportDriver::RegisterSendType(const transport::SendTypeWrapper& send_type_wrapper) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_) return false;
    MW_REQUIRE(state_.load() == State::kInit, "Method can only be called when state is 'Init'.");
    const auto& info = send_type_wrapper.info;
    std::string url = MakeStdTransportUrl(info.topic_name, info.msg_type);
    if (bus_.RegisterPublisher(url) == nullptr) return false;
    MW_INFO("Register publish type to shm transport, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  } catch (...) {
    MW_ERROR("RegisterSendType failed with an unknown exception.");
    return false;
  }
}

bool ShmTransportDriver::RegisterLoanedPodTopic(const transport::TransportInfo& info, size_t initial_size_hint,
                                                size_t alignment) noexcept {
  if (state_.load() != State::kInit) {
    MW_ERROR("Loaned POD topics can only be registered in Init state.");
    return false;
  }
  return EnsurePodTopic(info.topic_name, info.msg_type, initial_size_hint, alignment);
}

bool ShmTransportDriver::OnMessage(const transport::MessageHandlerWrapper& message_handler) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_) return false;
    MW_REQUIRE(state_.load() == State::kInit, "Method can only be called when state is 'Init'.");
    auto operation = bus_.TryAcquireOperation();
    if (!operation) return false;

    namespace util = ibmw::common::util;
    const auto& info = message_handler.info;
    std::string url = MakeStdTransportUrl(info.topic_name, info.msg_type);

    if (auto it = message_handler_map_.find(url); it != message_handler_map_.end()) {
      it->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool = std::make_shared<transport::MessageHandlerTool>();
    msg_handler_tool->AddMessageHandlerWrapper(&message_handler);

    auto sched_info_set = std::make_shared<std::atomic_bool>(false);
    auto handle = [sched_info_set, listener_options = options_, msg_handler_tool, driver_name = name_](
                      const void* payload, size_t payload_size, std::function<void()>&& release) {
      const auto validated_pkg_size = ValidateEncodedPayloadSize(payload, payload_size);
      if (!validated_pkg_size) {
        MW_WARN("Discard malformed shm transport chunk: size {}.", payload_size);
        if (release) release();
        return;
      }
      try {
        // Thread options apply to whichever listener thread delivers first.
        if (!sched_info_set->exchange(true)) [[unlikely]] {
          if (!listener_options.listener_thread_name.empty()) {
            runtime::core::util::SetNameForCurrentThread(listener_options.listener_thread_name);
          }
          if (!listener_options.listener_thread_bind_cpu.empty()) {
            runtime::core::util::BindCpuForCurrentThread(listener_options.listener_thread_bind_cpu);
          }
          if (!listener_options.listener_thread_sched_policy.empty()) {
            runtime::core::util::SetCpuSchedForCurrentThread(listener_options.listener_thread_sched_policy);
          }
        }
        // pkg_size | serialization_type | ctx_num | (ctx_key | ctx_val)* | msg_buffer
        auto ctx_ptr = std::make_shared<ibmw::transport::Context>(
            ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);
        const uint32_t pkg_size = *validated_pkg_size;
        util::ConstBufferOperator buf_oper(static_cast<const char*>(payload) + 4, pkg_size - 4);

        std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
        ctx_ptr->SetSerializationType(serialization_type);

        size_t ctx_num = buf_oper.GetUint8();
        for (size_t ii = 0; ii < ctx_num; ++ii) {
          auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
          auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
          ctx_ptr->SetMetaValue(key, val);
        }
        ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, driver_name);

        auto remaining_buf = buf_oper.GetRemainingBuffer();
        msg_handler_tool->DoMessageCallback(ctx_ptr, serialization_type,
                                            static_cast<const void*>(remaining_buf.data()), remaining_buf.size());
      } catch (const std::exception& e) {
        MW_WARN("Handle shm transport msg failed, exception info: {}", e.what());
      } catch (...) {
        MW_WARN("Handle shm transport msg failed with an unknown exception.");
      }
      if (release) release();
    };

    auto registration = bus_.RegisterSubscriber(url, std::move(handle));
    try {
      if (!message_handler_map_.emplace(url, std::move(msg_handler_tool)).second) {
        throw std::runtime_error("Shm message handler URL is already registered");
      }
    } catch (...) {
      registration->Stop();
      throw;
    }
    MW_INFO("Register subscribe type to shm transport, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

// Framed publish. The loan starts at the publisher's size hint and doubles
// while serialization runs out of space; the size that fitted becomes the
// next hint, so steady-state messages need a single loan.
void ShmTransportDriver::Send(transport::MessageFrame& msg_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kStart, "Method can only be called when state is 'Start'.");
    auto operation = bus_.TryAcquireOperation();
    if (!operation) return;
    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;
    std::string url = MakeStdTransportUrl(info.topic_name, info.msg_type);
    auto* pub = bus_.GetPublisher(operation, url);
    MW_REQUIRE(pub != nullptr, "Url: {} not registered for publishing!", url);

    std::string_view serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty()) serialization_type = info.msg_type_support_ref.DefaultSerializationType();
    MW_REQUIRE(serialization_type.size() <= std::numeric_limits<uint8_t>::max(),
               "Serialization type '{}' is too long.", serialization_type);

    auto [meta_key_vals_array, meta_key_vals_array_len] = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    MW_REQUIRE(meta_key_vals_array_len / 2 <= 255, "Too much context meta, require less than 255, but actually {}.",
               meta_key_vals_array_len / 2);
    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      MW_REQUIRE(meta_key_vals_array[ii].len <= std::numeric_limits<uint16_t>::max(), "Context meta is too long.");
      context_meta_kv_size += 2 + meta_key_vals_array[ii].len;
    }
    const size_t min_shm_size = 4 + 1 + serialization_type.size() + context_meta_kv_size;

    auto write_header = [&](util::BufferOperator& buf_oper) {
      buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
      buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
      for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
        buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
      }
    };

    auto& serialization_cache = msg_wrapper.serialization_cache;
    if (auto finditr = serialization_cache.find(serialization_type); finditr != serialization_cache.end()) {
      const auto& view = finditr->second;
      const auto* buffers = view->Data();
      const size_t buffer_count = view->Size();
      size_t total_size = 0;
      MW_REQUIRE(CheckedAddSize(min_shm_size, view->BufferSize(), total_size) &&
                     total_size <= std::numeric_limits<uint32_t>::max(),
                 "Message on '{}' is too large for shm.", url);
      void* chunk = pub->Loan(total_size);
      MW_REQUIRE(chunk != nullptr, "Shm loan of {} bytes on '{}' failed.", total_size, url);
      LoanGuard guard([pub, chunk] { pub->Release(chunk); });
      util::BufferOperator buf_oper(static_cast<char*>(chunk), total_size);
      buf_oper.SetUint32(static_cast<uint32_t>(total_size));
      write_header(buf_oper);
      for (size_t ii = 0; ii < buffer_count; ++ii) {
        buf_oper.SetBuffer(static_cast<const char*>(buffers[ii].data), buffers[ii].len);
      }
      guard.Disarm();
      pub->Publish(chunk);
      MW_TRACE("Shm publish to '{}'", url);
      return;
    }

    CheckMsg(msg_wrapper);

    auto loan_size = NextShmLoanSize(pub->SizeHint(), min_shm_size);
    MW_REQUIRE(loan_size.has_value(), "Message header on '{}' is too large for shm.", url);
    while (true) {
      void* chunk = pub->Loan(*loan_size);
      MW_REQUIRE(chunk != nullptr, "Shm loan of {} bytes on '{}' failed.", *loan_size, url);
      LoanGuard guard([pub, chunk] { pub->Release(chunk); });

      util::BufferOperator buf_oper(static_cast<char*>(chunk), *loan_size);
      buf_oper.Skip(4);
      write_header(buf_oper);

      ibmw::util::FlatBufferArrayAllocator allocator(static_cast<char*>(chunk) + min_shm_size,
                                                     *loan_size - min_shm_size);
      ibmw::util::BufferArray buffer_array(allocator.NativeHandle());
      const bool serialize_ret =
          info.msg_type_support_ref.Serialize(serialization_type, msg_wrapper.msg_ptr,
                                              buffer_array.AllocatorNativeHandle(),
                                              buffer_array.BufferArrayNativeHandle());
      if (!serialize_ret) [[unlikely]] {
        MW_REQUIRE(allocator.IsOutOfMemory(), "Serialize failed.");
        loan_size = NextShmLoanSize(*loan_size, *loan_size + 1);
        MW_REQUIRE(loan_size.has_value(), "Message on '{}' is too large for shm.", url);
        continue;
      }

      buf_oper.JumpTo(0);
      buf_oper.SetUint32(static_cast<uint32_t>(min_shm_size + buffer_array.BufferSize()));
      pub->RaiseSizeHint(*loan_size);
      guard.Disarm();
      pub->Publish(chunk);
      MW_TRACE("Shm publish to '{}'", url);
      return;
    }
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  } catch (...) {
    MW_ERROR("Shm send threw an unknown exception.");
  }
}

bool ShmTransportDriver::EnsurePodTopic(std::string_view topic, std::string_view msg_type, size_t initial_size_hint,
                                        size_t alignment) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_ || state_.load() != State::kInit) return false;
    auto operation = bus_.TryAcquireOperation();
    if (!operation) return false;
    if (initial_size_hint == 0 || initial_size_hint > std::numeric_limits<uint32_t>::max() || alignment == 0 ||
        alignment > shm::kMaxPayloadAlignment || (alignment & (alignment - 1)) != 0) {
      MW_ERROR("EnsurePodTopic: invalid size/alignment ({}, {})", initial_size_hint, alignment);
      return false;
    }
    auto key = MakePodKey(topic, msg_type);
    if (const auto existing = pod_pub_map_.find(key); existing != pod_pub_map_.end()) {
      return existing->second.initial_size_hint == initial_size_hint && existing->second.alignment == alignment;
    }
    auto url = MakePodUrl(topic, msg_type);
    auto* pub = bus_.RegisterPublisher(url, initial_size_hint);
    if (pub == nullptr) {
      MW_ERROR("EnsurePodTopic: failed to obtain publisher for url={}", url);
      return false;
    }
    pod_pub_map_.emplace(std::move(key), PodPublisherEndpoint{pub, initial_size_hint, alignment});
    MW_INFO("Registered shm zero-copy POD publisher, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("EnsurePodTopic exception: {}", e.what());
    return false;
  }
}

bool ShmTransportDriver::RegisterPodSubscriber(std::string_view topic, std::string_view msg_type,
                                               PodMessageCallback callback) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  try {
    if (shutdown_started_ || state_.load() != State::kInit) return false;
    auto url = MakePodUrl(topic, msg_type);
    auto handle = [cb = std::move(callback)](const void* payload, size_t payload_size,
                                             std::function<void()>&& release) {
      try {
        cb(payload, payload_size);
      } catch (const std::exception& e) {
        MW_WARN("POD subscriber callback threw: {}", e.what());
      } catch (...) {
        MW_WARN("POD subscriber callback threw an unknown exception.");
      }
      if (release) release();
    };
    bus_.RegisterSubscriber(url, std::move(handle));
    MW_INFO("Registered shm zero-copy POD subscriber, url: {}", url);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("RegisterPodSubscriber exception: {}", e.what());
    return false;
  }
}

bool ShmTransportDriver::AttachLoaned(const transport::LoanedMessageHandlerWrapper& message_handler) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("AttachLoaned called outside Init state.");
    return false;
  }
  if (shutdown_started_) return false;
  try {
    const auto& info = message_handler.info;
    auto handle = [loaned_cb = message_handler.callback](const void* payload, size_t size,
                                                          std::function<void()>&& release) {
      loaned_cb(/*ctx_ptr=*/nullptr, payload, size, std::move(release));
    };
    return static_cast<bool>(bus_.RegisterSubscriber(MakePodUrl(info.topic_name, info.msg_type), std::move(handle)));
  } catch (const std::exception& e) {
    MW_ERROR("AttachLoaned exception: {}", e.what());
    return false;
  }
}

std::unique_ptr<transport::LoanedSubscriptionHandle> ShmTransportDriver::AttachLoanedRetained(
    const transport::LoanedMessageHandlerWrapper& message_handler, transport::RetainedLoanOptions options) noexcept {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (state_.load() != State::kInit || options.max_outstanding == 0 ||
      options.max_outstanding > shm::kMaxChunksHeldPerSubscriber) {
    MW_ERROR("Invalid retained loan max_outstanding {}; shm supports 1..{}.", options.max_outstanding,
             shm::kMaxChunksHeldPerSubscriber);
    return nullptr;
  }
  if (shutdown_started_) return nullptr;
  std::string url;  // built inside the try: allocation may throw in this noexcept function
  try {
    url = MakePodUrl(message_handler.info.topic_name, message_handler.info.msg_type);
    if (!bus_.ReserveSubscriberUrl(url)) return nullptr;
    ShmSubscriptionOptions subscription_options;
    subscription_options.retained = true;
    subscription_options.max_outstanding = options.max_outstanding;
    subscription_options.url_reserved = true;
    auto subscription = bus_.RegisterSubscriber(
        url,
        [callback = message_handler.callback](const void* payload, size_t size, std::function<void()>&& release) {
          callback(nullptr, payload, size, std::move(release));
        },
        subscription_options);
    return std::make_unique<ShmLoanedSubscriptionHandle>(std::move(subscription));
  } catch (const std::exception& e) {
    if (!url.empty()) bus_.RollbackSubscriberUrl(url);
    MW_ERROR("AttachLoanedRetained exception: {}", e.what());
    return nullptr;
  } catch (...) {
    if (!url.empty()) bus_.RollbackSubscriberUrl(url);
    MW_ERROR("AttachLoanedRetained failed with an unknown exception.");
    return nullptr;
  }
}

bool ShmTransportDriver::LoanCtxMeta(ShmPublisher* pub, const transport::TransportInfo& info, size_t requested_size,
                                     const ibmw::transport::Context& ctx, transport::LoanedSampleHandle& out_handle) {
  const std::string_view serialization_type = SerializationTypeOf(ctx, info);
  const auto kv_map = ctx.GetMetaKeyVals();
  const size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv_map);
  if (header_size == 0) return false;
  size_t total_size = 0;
  if (!CheckedAddSize(header_size, requested_size, total_size) || total_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  ShmLoanSlot* slot = pub->LoanSlot(total_size);
  if (slot == nullptr) return false;
  if (EncodeCtxMetaHeader(slot->Chunk(), total_size, static_cast<uint32_t>(total_size), serialization_type,
                          kv_map) != header_size) {
    pub->ReleaseSlot(slot->Token());
    return false;
  }
  out_handle.user_payload_ptr = static_cast<char*>(slot->Chunk()) + header_size;
  out_handle.user_payload_size = requested_size;
  out_handle.mode = transport::LoanMode::kCtxMetaThenPayloadUnreported;
  out_handle.driver_token = slot->Token();
  out_handle.source_driver = this;
  return true;
}

bool ShmTransportDriver::LoanSample(const transport::TransportInfo& info, size_t requested_size,
                                    transport::LoanMode mode, const ibmw::transport::Context* ctx,
                                    transport::LoanedSampleHandle& out_handle) noexcept {
  if (mode == transport::LoanMode::kCtxMetaThenPayload) {
    if (ctx == nullptr) {
      MW_WARN("LoanSample: kCtxMetaThenPayload requires non-null ctx.");
      return false;
    }
    try {
      auto operation = bus_.TryAcquireOperation();
      if (!operation) return false;
      auto* pub = bus_.GetPublisher(operation, MakeStdTransportUrl(info.topic_name, info.msg_type));
      if (pub == nullptr) {
        MW_WARN("LoanSample[ctx-meta]: standard topic not registered. Call RegisterSendType() for topic={} "
                "msg_type={} first.",
                info.topic_name, info.msg_type);
        return false;
      }
      return LoanCtxMeta(pub, info, requested_size, *ctx, out_handle);
    } catch (const std::exception& e) {
      MW_ERROR("LoanSample[ctx-meta] exception: {}", e.what());
      return false;
    }
  }

  try {
    auto operation = bus_.TryAcquireOperation();
    if (!operation) return false;
    // pod_pub_map_ is only written in Init state, before any Start-phase loan.
    auto it = pod_pub_map_.find(MakePodKey(info.topic_name, info.msg_type));
    if (it == pod_pub_map_.end()) {
      MW_WARN("LoanSample: POD topic not registered. Call EnsurePodTopic first. topic={} msg_type={}",
              info.topic_name, info.msg_type);
      return false;
    }
    // A slot, like ctx-meta loans: its tagged token tells a stale handle
    // from a new loan of the same chunk.
    ShmLoanSlot* slot = it->second.publisher->LoanSlot(requested_size, it->second.alignment);
    if (slot == nullptr) return false;
    out_handle.user_payload_ptr = slot->Chunk();
    out_handle.user_payload_size = requested_size;
    out_handle.mode = transport::LoanMode::kPodOnly;
    out_handle.driver_token = slot->Token();
    out_handle.source_driver = this;
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("LoanSample exception: {}", e.what());
    return false;
  }
}

bool ShmTransportDriver::LoanSerialized(ShmPublisher* pub, const transport::TransportInfo& info,
                                        const ibmw::transport::Context& ctx, const void* msg_ptr,
                                        transport::LoanedSampleHandle& out_handle) {
  const std::string_view serialization_type = SerializationTypeOf(ctx, info);
  const auto kv_map = ctx.GetMetaKeyVals();
  const size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv_map);
  if (header_size == 0) return false;
  size_t chunk_size = 0;
  if (!CheckedAddSize(header_size, kInitialPayloadHint, chunk_size) ||
      chunk_size > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  for (size_t attempt = 0; attempt < kMaxLoanRetries; ++attempt) {
    ShmLoanSlot* slot = pub->LoanSlot(chunk_size);
    if (slot == nullptr) {
      MW_WARN("LoanSerializedSample: loan of {} bytes returned null on attempt {}", chunk_size, attempt);
      return false;
    }
    void* const token = slot->Token();
    void* const chunk = slot->Chunk();
    LoanGuard guard([pub, token] { pub->ReleaseSlot(token); });
    ibmw::util::FlatBufferArrayAllocator allocator(static_cast<char*>(chunk) + header_size,
                                                   chunk_size - header_size);
    ibmw::util::BufferArray buffer_array(allocator.NativeHandle());
    if (info.msg_type_support_ref.Serialize(serialization_type, msg_ptr, buffer_array.AllocatorNativeHandle(),
                                            buffer_array.BufferArrayNativeHandle())) {
      const size_t payload_size = buffer_array.BufferSize();
      size_t total_size = 0;
      if (!CheckedAddSize(header_size, payload_size, total_size) ||
          total_size > std::numeric_limits<uint32_t>::max() ||
          EncodeCtxMetaHeader(chunk, chunk_size, static_cast<uint32_t>(total_size), serialization_type,
                              kv_map) != header_size) {
        return false;
      }
      out_handle.user_payload_ptr = static_cast<char*>(chunk) + header_size;
      out_handle.user_payload_size = payload_size;
      out_handle.mode = transport::LoanMode::kCtxMetaThenPayloadUnreported;
      out_handle.driver_token = token;
      out_handle.source_driver = this;
      guard.Disarm();
      return true;
    }
    if (!allocator.IsOutOfMemory()) {
      MW_ERROR("LoanSerializedSample: serialize failed for non-OOM reason (topic={} msg_type={})", info.topic_name,
               info.msg_type);
      return false;
    }
    if (chunk_size > std::numeric_limits<uint32_t>::max() / 2) break;
    chunk_size *= 2;
  }
  MW_ERROR("LoanSerializedSample: gave up after {} growth retries (final chunk_size={})", kMaxLoanRetries,
           chunk_size);
  return false;
}

bool ShmTransportDriver::LoanSerializedSample(const transport::TransportInfo& info,
                                              const ibmw::transport::Context& ctx, const void* msg_ptr,
                                              transport::LoanedSampleHandle& out_handle) noexcept {
  try {
    auto operation = bus_.TryAcquireOperation();
    if (!operation) return false;
    if (msg_ptr == nullptr) return false;
    if (!info.msg_type_support_ref) {
      MW_WARN("LoanSerializedSample: msg_type_support_ref is empty -- caller must populate it.");
      return false;
    }
    auto* pub = bus_.GetPublisher(operation, MakeStdTransportUrl(info.topic_name, info.msg_type));
    if (pub == nullptr) {
      MW_WARN("LoanSerializedSample: standard topic not registered. Call RegisterSendType() for topic={} "
              "msg_type={} first.",
              info.topic_name, info.msg_type);
      return false;
    }
    return LoanSerialized(pub, info, ctx, msg_ptr, out_handle);
  } catch (const std::exception& e) {
    MW_ERROR("LoanSerializedSample exception: {}", e.what());
    return false;
  } catch (...) {
    MW_ERROR("LoanSerializedSample threw an unknown exception.");
    return false;
  }
}

size_t ShmTransportDriver::ComputeCtxMetaHeaderSize(
    std::string_view serialization_type, const std::unordered_map<std::string_view, std::string_view>& kv) noexcept {
  if (serialization_type.size() > std::numeric_limits<uint8_t>::max()) return 0;
  if (kv.size() > std::numeric_limits<uint8_t>::max()) return 0;
  size_t size = 4 + 1 + serialization_type.size() + 1;
  for (const auto& [k, v] : kv) {
    if (k.size() > std::numeric_limits<uint16_t>::max() || v.size() > std::numeric_limits<uint16_t>::max()) return 0;
    size += 2 + k.size() + 2 + v.size();
  }
  return size;
}

size_t ShmTransportDriver::EncodeCtxMetaHeader(
    void* buffer, size_t buffer_size, uint32_t total_size, std::string_view serialization_type,
    const std::unordered_map<std::string_view, std::string_view>& kv) noexcept {
  try {
    namespace util = ibmw::common::util;
    const size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv);
    if (header_size == 0 || buffer_size < header_size) return 0;
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

void ShmTransportDriver::PublishLoaned(transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr || handle.source_driver != this) return;
  switch (handle.mode) {
    case transport::LoanMode::kCtxMetaThenPayloadUnreported:
    case transport::LoanMode::kPodOnly:
      ShmLoanSlot::FromToken(handle.driver_token)->owner->PublishSlot(handle.driver_token);
      break;
    default:
      return;
  }
  ClearHandle(handle);
}

void ShmTransportDriver::ReleaseLoaned(transport::LoanedSampleHandle& handle) noexcept {
  if (handle.driver_token == nullptr || handle.user_payload_ptr == nullptr || handle.source_driver != this) return;
  switch (handle.mode) {
    case transport::LoanMode::kCtxMetaThenPayloadUnreported:
    case transport::LoanMode::kPodOnly:
      ShmLoanSlot::FromToken(handle.driver_token)->owner->ReleaseSlot(handle.driver_token);
      break;
    default:
      return;
  }
  ClearHandle(handle);
}

}  // namespace ibmw::extensions::shm_extension
