// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/capture/cached_transport_subscription_source.h"

#include <charconv>
#include <cstdint>

#include "base/exception.h"
#include "base/time_util.h"
#include "engine/transport/transport_driver_tools.h"
#include "record_playback_ext/global.h"
#include "record_playback_ext/recorder/record_payload.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

uint64_t ParseUint64(std::string_view value) {
  uint64_t result = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc() ? result : 0;
}

std::string SubscriptionKey(
    const ibmw::runtime::core::transport::TransportDriver& driver,
    const TopicAdvertisement& advertisement) {
  return std::to_string(reinterpret_cast<uintptr_t>(&driver)) + '\0' +
         advertisement.topic.topic + '\0' + advertisement.topic.msg_type +
         '\0' + advertisement.topic.serialization_type + '\0' +
         advertisement.topic.schema.schema_hash;
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

  ~InFlightGuard() {
    if (!armed) {
      return;
    }
    std::lock_guard lock(state->mutex);
    --state->in_flight;
    if (state->in_flight == 0) {
      state->idle_cv.notify_all();
    }
  }
};

void CaptureFrame(
    const std::shared_ptr<SubscriptionState>& state,
    ibmw::runtime::core::transport::MessageFrame& frame,
    bool loaned_payload,
    std::function<void()>&& release) {
  InFlightGuard in_flight_guard{state};
  ReleaseGuard release_guard{std::move(release)};
  TopicAdvertisement advertisement;
  std::string driver_name;
  ibmw::transport::SerializedMessageCallback callback;
  try {
    {
      std::lock_guard lock(state->mutex);
      if (!state->active) {
        return;
      }
      ++state->in_flight;
      in_flight_guard.armed = true;
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
    auto payload = AcquireRecordPayload(
        view, advertisement.topic.serialization_type,
        RecordTranscodePolicy::kAllow);
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
    MW_ERROR(
        "Cached {} capture failed for topic '{}': {}.", driver_name,
        advertisement.topic.topic, error.what());
    std::lock_guard lock(state->mutex);
    if (!state->error) {
      state->error = std::current_exception();
    }
    state->active = false;
  } catch (...) {
    MW_ERROR(
        "Cached {} capture failed for topic '{}' with an unknown error.",
        driver_name, advertisement.topic.topic);
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
    ibmw::runtime::core::transport::TransportDriver& driver,
    const TopicAdvertisement& advertisement,
    ibmw::util::TypeSupportRef type_support,
    ibmw::transport::SerializedMessageCallback callback) {
  MW_ASSERT(type_support, "Cached subscription requires TypeSupport.");
  MW_ASSERT(
      type_support.TypeName() == advertisement.topic.msg_type,
      "TypeSupport '{}' does not match advertised type '{}'.",
      type_support.TypeName(), advertisement.topic.msg_type);

  const std::string subscription_key = SubscriptionKey(driver, advertisement);
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
      state->error = nullptr;
      state->active = true;
      return state;
    }
  }

  auto state = std::make_shared<SubscriptionState>();
  state->callback = std::move(callback);
  state->advertisement = advertisement;
  state->driver_name = std::move(driver_name);
  state->active = true;
  auto handler = std::make_unique<
      ibmw::runtime::core::transport::MessageHandlerWrapper>();
  handler->info = {
      .msg_type = advertisement.topic.msg_type,
      .topic_name = advertisement.topic.topic,
      .pkg_path = advertisement.type_support_package,
      .module_name = "ibmw-bag-recorder",
      .msg_type_support_ref = type_support};
  handler->require_cache_serialization_types.emplace(
      advertisement.topic.serialization_type);
  handler->callback = [state](
                          ibmw::runtime::core::transport::MessageFrame& frame,
                          std::function<void()>&& release) {
        CaptureFrame(state, frame, false, std::move(release));
      };

  MW_ASSERT(
      driver.OnMessage(*handler),
      "Driver '{}' rejected cached serialized subscription for '{}'.",
      driver.Name(), advertisement.topic.topic);
  auto loaned_handler = std::make_unique<
      ibmw::runtime::core::transport::LoanedMessageHandlerWrapper>();
  loaned_handler->info = handler->info;
  loaned_handler->callback =
      [state, info = loaned_handler->info](
          const ibmw_transport_context_base_t* context,
          const void* payload,
          size_t payload_size,
          std::function<void()>&& release) {
        static_cast<void>(payload_size);
        ibmw::runtime::core::transport::MessageFrame frame{
            .info = info,
            .msg_ptr = payload,
            .ctx_ref = ibmw::transport::ContextRef(context)};
        CaptureFrame(state, frame, true, std::move(release));
      };
  const bool loaned_attached = driver.AttachLoaned(*loaned_handler);
  {
    std::lock_guard lock(mutex_);
    states_.emplace(subscription_key, state);
    handlers_.emplace_back(std::move(handler));
    if (loaned_attached) {
      loaned_handlers_.emplace_back(std::move(loaned_handler));
    }
  }
  return state;
}

CachedTransportSubscriptionSource::CachedTransportSubscriptionSource(
    std::string driver_name,
    ibmw::runtime::core::transport::TransportDriver& driver,
    TopicAdvertisement advertisement,
    ibmw::util::TypeSupportRef type_support,
    CachedTransportSubscriptionRegistry& registry)
    : driver_name_(std::move(driver_name)),
      driver_(driver),
      advertisement_(std::move(advertisement)),
      type_support_(type_support),
      registry_(registry) {}

CachedTransportSubscriptionSource::~CachedTransportSubscriptionSource() {
  try {
    Stop();
  } catch (...) {
  }
}

void CachedTransportSubscriptionSource::Start(
    ibmw::transport::SerializedMessageCallback callback) {
  state_ = registry_.Attach(
      driver_name_, driver_, advertisement_, type_support_,
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
    state_->idle_cv.wait(lock, [this] { return state_->in_flight == 0; });
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

}  // namespace ibmw::extensions::record_playback_extension
