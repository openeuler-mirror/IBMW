// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/cached_transport_subscription_source.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <thread>

#include "base/exception.h"
#include "base/time_util.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/topic_io/serialized_payload.h"

namespace ibmw::runtime::core::topic_io {

namespace {

uint64_t ParseUint64(std::string_view value) {
  uint64_t result = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc() ? result : 0;
}

std::string SubscriptionKey(
    const ibmw::runtime::core::transport::TransportDriver& driver,
    const TopicAdvertisement& advertisement,
    std::string_view module_name,
    const ibmw::transport::QosDescriptor& qos) {
  return std::to_string(reinterpret_cast<uintptr_t>(&driver)) + '\0' +
         std::string(module_name) + '\0' +
         advertisement.topic.topic + '\0' + advertisement.topic.msg_type +
         '\0' + advertisement.topic.serialization_type + '\0' +
         advertisement.topic.schema.schema_hash + '\0' + advertisement.type_support_package + '\0' +
         nlohmann::json(qos.policies).dump();
}

using SubscriptionState =
    CachedTransportSubscriptionRegistry::SubscriptionState;

struct ReleaseGuard {
  std::function<void()> callback;

  ~ReleaseGuard() {
    if (callback) {
      callback();
    }
  }
};

struct InFlightGuard {
  std::shared_ptr<SubscriptionState> state;
  bool armed = false;
  SubscriptionState::Delivery delivery;

  // Called with state->mutex held.
  void Arm() {
    delivery.thread = std::this_thread::get_id();
    delivery.next = state->deliveries;
    if (delivery.next) delivery.next->prev = &delivery;
    state->deliveries = &delivery;
    ++state->in_flight;
    armed = true;
  }

  ~InFlightGuard() {
    if (!armed) {
      return;
    }
    std::lock_guard lock(state->mutex);
    --state->in_flight;
    (delivery.prev ? delivery.prev->next : state->deliveries) = delivery.next;
    if (delivery.next) delivery.next->prev = delivery.prev;
    if (state->in_flight == 0) {
      state->idle_cv.notify_all();
    }
  }
};

// Waits until only the deliveries running on this thread (Stop() from a
// callback) are left. Cold and out of line: Stop() is not on the delivery path.
[[gnu::cold, gnu::noinline]] void WaitForOtherDeliveries(
    SubscriptionState& state, std::unique_lock<std::mutex>& lock) {
  size_t own_depth = 0;
  for (auto* d = state.deliveries; d; d = d->next) {
    own_depth += d->thread == std::this_thread::get_id();
  }
  while (!state.idle_cv.wait_for(lock, std::chrono::seconds(5), [&] {
    return state.in_flight <= own_depth;
  })) {
    // Returning early would let a running callback outlive its owner.
    if (state.logger) {
      MW_LOG(*state.logger, ibmw::common::util::kLogLevelWarn,
             "Stopping cached {} subscription for topic '{}': still waiting for {} running callback(s).",
             state.driver_name, state.advertisement.topic.topic, state.in_flight - own_depth);
    }
  }
}

void DeliverFrame(
    const std::shared_ptr<SubscriptionState>& state,
    ibmw::runtime::core::transport::MessageFrame& frame,
    bool loaned_payload,
    std::function<void()>&& release) {
  InFlightGuard in_flight_guard{state};
  ReleaseGuard release_guard{std::move(release)};
  TopicAdvertisement advertisement;
  std::string driver_name;
  ibmw::transport::SerializedMessageCallback callback;
  const ibmw::common::util::LoggerWrapper* logger = nullptr;
  try {
    {
      std::lock_guard lock(state->mutex);
      if (!state->active) {
        return;
      }
      in_flight_guard.Arm();
      logger = state->logger;
      advertisement = state->advertisement;
      driver_name = state->driver_name;
      callback = state->callback;
    }
    ibmw::extension::MessageView view{
        .topic = frame.info.topic_name,
        .msg_type = frame.info.msg_type,
        .module_name = frame.info.module_name,
        .endpoint_index = frame.info.index,
        .context = frame.ctx_ref,
        .has_typed_message = frame.msg_ptr != nullptr,
        .available_serialization_types = [&] {
          std::unordered_set<std::string> formats;
          for (const auto& [format, cached] : frame.serialization_cache) {
            static_cast<void>(cached);
            formats.emplace(format);
          }
          return formats;
        }(),
        .acquire_serialized = [&frame](std::string_view format) {
          return ibmw::runtime::core::transport::TrySerializeMsgWithCache(
              frame, format);
        }};
    auto payload = AcquireSerializedPayload(
        view, advertisement.topic.serialization_type,
        SerializedPayloadPolicy::kAllow);
    if (!payload) {
      return;
    }
    const std::string_view sequence_text =
        frame.ctx_ref
            ? frame.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ)
            : std::string_view{};
    const uint64_t sequence =
        loaned_payload && sequence_text.empty()
            ? state->loaned_sequence.fetch_add(1, std::memory_order_relaxed) + 1
            : ParseUint64(sequence_text);
    const uint64_t publish_timestamp =
        frame.ctx_ref
            ? ParseUint64(frame.ctx_ref.GetMetaValue(
                  IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP))
            : 0;
    callback(
        ibmw::transport::SerializedMessage{
            .topic = advertisement.topic,
            .identity =
                {.source_id = advertisement.runtime_id,
                 .publisher_id = advertisement.publisher_id,
                 .sequence = sequence},
            .publish_timestamp = publish_timestamp,
            .receive_timestamp = ibmw::common::util::GetCurTimestampNs(),
            .source_driver = driver_name,
            .payload = std::move(payload)});
  } catch (const std::exception& error) {
    if (logger) {
      MW_LOG(
          *logger, ibmw::common::util::kLogLevelError,
          "Cached {} subscription failed for topic '{}': {}.", driver_name,
          advertisement.topic.topic, error.what());
    }
    std::lock_guard lock(state->mutex);
    if (!state->error) {
      state->error = std::current_exception();
    }
    state->active = false;
  } catch (...) {
    if (logger) {
      MW_LOG(
          *logger, ibmw::common::util::kLogLevelError,
          "Cached {} subscription failed for topic '{}' with an unknown error.",
          driver_name, advertisement.topic.topic);
    }
    std::lock_guard lock(state->mutex);
    if (!state->error) {
      state->error = std::current_exception();
    }
    state->active = false;
  }
}

}  // namespace

std::shared_ptr<CachedTransportSubscriptionRegistry::SubscriptionState>
CachedTransportSubscriptionRegistry::Attach(
    std::string driver_name,
    std::string module_name,
    ibmw::runtime::core::transport::TransportDriver& driver,
    const TopicAdvertisement& advertisement,
    ibmw::util::TypeSupportRef type_support,
    const ibmw::transport::QosDescriptor& requested_qos,
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(type_support, "Cached subscription requires TypeSupport.");
  MW_ASSERT(
      type_support.TypeName() == advertisement.topic.msg_type,
      "TypeSupport '{}' does not match advertised type '{}'.",
      type_support.TypeName(), advertisement.topic.msg_type);

  const std::string subscription_key =
      SubscriptionKey(driver, advertisement, module_name, requested_qos);
  {
    std::lock_guard registry_lock(mutex_);
    const auto existing = states_.find(subscription_key);
    if (existing != states_.end()) {
      auto& state = existing->second;
      std::lock_guard state_lock(state->mutex);
      MW_ASSERT(
          !state->active && state->in_flight == 0,
          "Cached subscription for '{}' is already active.",
          advertisement.topic.topic);
      state->callback = std::move(callback);
      state->advertisement = advertisement;
      state->driver_name = std::move(driver_name);
      state->logger = &logger_;
      state->error = nullptr;
      state->active = true;
      return state;
    }
  }

  auto state = std::make_shared<SubscriptionState>();
  state->callback = std::move(callback);
  state->advertisement = advertisement;
  state->driver_name = std::move(driver_name);
  state->logger = &logger_;
  state->active = true;
  auto handler = std::make_unique<
      ibmw::runtime::core::transport::MessageHandlerWrapper>();
  handler->info = {
      .msg_type = advertisement.topic.msg_type,
      .topic_name = advertisement.topic.topic,
      .pkg_path = advertisement.type_support_package,
      .module_name = std::move(module_name),
      .msg_type_support_ref = type_support};
  handler->requested_qos = requested_qos;
  handler->require_cache_serialization_types.emplace(
      advertisement.topic.serialization_type);
  handler->callback =
      [state](ibmw::runtime::core::transport::MessageFrame& frame,
              std::function<void()>&& release,
              const ibmw::runtime::core::transport::internal::CallbackInvocationEnvelopeV1*) {
        DeliverFrame(state, frame, false, std::move(release));
      };

  // The driver keeps references to the handlers, so they are owned by this
  // source before the driver sees them: a later throw (rejection, bad_alloc)
  // must not destroy a handler the driver may still call.
  auto* handler_ptr = handler.get();
  {
    std::lock_guard lock(mutex_);
    handlers_.emplace_back(std::move(handler));
  }
  try {
    MW_ASSERT(
        driver.OnMessage(*handler_ptr),
        "Driver '{}' rejected cached serialized subscription for '{}'.",
        driver.Name(), advertisement.topic.topic);
    auto loaned_handler = std::make_unique<
        ibmw::runtime::core::transport::LoanedMessageHandlerWrapper>();
    loaned_handler->info = handler_ptr->info;
    loaned_handler->callback =
        [state, info = loaned_handler->info, min_size = type_support.NativeHandle()->msg_min_size](
            const ibmw_transport_context_base_t* context,
            const void* payload,
            size_t payload_size,
            std::function<void()>&& release) {
          // The payload is read as one typed message: a loan smaller than the
          // type, or of a type whose size is unknown, is dropped (COMM-024).
          if (payload == nullptr || min_size == 0 || payload_size < min_size) [[unlikely]] {
            ReleaseGuard release_guard{std::move(release)};
            if (!state->undersized_loan_reported.exchange(true, std::memory_order_relaxed)) {
              const ibmw::common::util::LoggerWrapper* logger = nullptr;
              {
                std::lock_guard lock(state->mutex);
                logger = state->logger;
              }
              if (logger != nullptr) {
                MW_LOG(*logger, ibmw::common::util::kLogLevelWarn,
                       "Dropping loaned message of {} bytes on topic '{}': type '{}' needs {} bytes{}. "
                       "Further drops on this subscription are not reported.",
                       payload_size, info.topic_name, info.msg_type, min_size,
                       min_size == 0 ? " (size unknown)" : "");
              }
            }
            return;
          }
          ibmw::runtime::core::transport::MessageFrame frame{
              .info = info,
              .msg_ptr = payload,
              .ctx_ref = ibmw::transport::ContextRef(context)};
          DeliverFrame(state, frame, true, std::move(release));
        };
    auto* loaned_handler_ptr = loaned_handler.get();
    {
      std::lock_guard lock(mutex_);
      loaned_handlers_.emplace_back(std::move(loaned_handler));
    }
    // Kept even if rejected: a driver may have stored the callback anyway.
    static_cast<void>(driver.AttachLoaned(*loaned_handler_ptr));
    {
      std::lock_guard lock(mutex_);
      states_.emplace(subscription_key, state);
    }
  } catch (...) {
    // The driver may still hold (and call) the handlers: make sure a failed
    // subscription never reaches the user callback.
    std::lock_guard lock(state->mutex);
    state->active = false;
    throw;
  }
  return state;
}

CachedTransportSubscriptionSource::CachedTransportSubscriptionSource(
    std::string driver_name,
    std::string module_name,
    ibmw::runtime::core::transport::TransportDriver& driver,
    TopicAdvertisement advertisement,
    ibmw::util::TypeSupportRef type_support,
    CachedTransportSubscriptionRegistry& registry,
    ibmw::transport::QosDescriptor requested_qos)
    : driver_name_(std::move(driver_name)),
      module_name_(std::move(module_name)),
      driver_(driver),
      advertisement_(std::move(advertisement)),
      type_support_(type_support),
      registry_(registry),
      requested_qos_(std::move(requested_qos)) {}

CachedTransportSubscriptionSource::~CachedTransportSubscriptionSource() {
  try {
    Stop();
  } catch (...) {
  }
}

void CachedTransportSubscriptionSource::Start(
    ibmw::transport::SerializedMessageCallback callback) {
  state_ = registry_.Attach(
      driver_name_, module_name_, driver_, advertisement_, type_support_,
      requested_qos_,
      std::move(callback));
}

void CachedTransportSubscriptionSource::Stop() {
  if (!state_) {
    return;
  }
  std::exception_ptr error;
  {
    std::unique_lock lock(state_->mutex);
    state_->active = false;
    WaitForOtherDeliveries(*state_, lock);
    error = state_->error;
  }
  if (error) {
    std::rethrow_exception(error);
  }
}

std::string CachedTransportSubscriptionSource::Description() const {
  return driver_name_ + " cached serialized subscriber (runtime " +
         advertisement_.runtime_id + ")";
}

}  // namespace ibmw::runtime::core::topic_io
