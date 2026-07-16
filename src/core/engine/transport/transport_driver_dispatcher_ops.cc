// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/transport_driver_dispatcher.h"

#include <charconv>
#include <string>
#include <vector>

#include "c/transport/transport_context_base.h"
#include "cpp/transport/transport_handle.h"
#include "engine/transport/transport_driver_tools.h"
#include "base/time_util.h"

namespace ibmw::runtime::core::transport {

namespace {
// Phase 7 §7.2 C3: avoid std::to_string heap alloc on every Send().
// Numeric meta values (pub_seq, pub_ts) live in fixed digit count so a
// stack buffer of 24 chars covers the full uint64 range. The
// SetMetaValue C op deep-copies the string_view into the Context's meta
// map, so the buffer can safely die when the helper returns.
inline void SetMetaUint64(ibmw::transport::ContextRef& ctx_ref,
                          std::string_view key,
                          uint64_t value) {
  char buf[24];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  ctx_ref.SetMetaValue(key, std::string_view(buf, static_cast<size_t>(ptr - buf)));
}
}  // namespace

bool TransportDriverDispatcher::OnMessage(MessageHandlerAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Msg can only be subscribed when state is 'Init'.");
    return false;
  }

  if (wrapper.msg_type_support == nullptr) [[unlikely]] {
    MW_ERROR("Msg type support is null.");
    return false;
  }

  if (wrapper.callback == nullptr) [[unlikely]] {
    MW_ERROR("Callback is null.");
    return false;
  }

  auto topic_name = wrapper.topic_name;
  auto msg_type_support_ref = ibmw::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();

  // create sub wrapper
  auto msg_handler_ptr = std::make_unique<MessageHandlerWrapper>();
  msg_handler_ptr->info = TransportInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++sub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, subscribe_filters_rules_);
  subscribe_filter_bus_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // set callback
  const auto& filter_collector = subscribe_filter_bus_ptr_->GetFilterCollector(topic_name);

  auto sub_func_shared_ptr = std::make_shared<ibmw::transport::MessageCallback>(wrapper.callback);

  msg_handler_ptr->callback =
      [this, &filter_collector, sub_func_shared_ptr](
          MessageFrame& msg_wrapper, std::function<void()>&& input_release_callback) {
        auto release_callback_shared_ptr = std::shared_ptr<std::function<void()>>(
            new std::function<void()>(std::move(input_release_callback)),
            [](std::function<void()>* f) {
              (*f)();
              delete f;
            });

        filter_collector.InvokeTransport(
            [this,
             sub_func_ptr = sub_func_shared_ptr.get(),
             release_callback_shared_ptr](MessageFrame& msg_wrapper) {
              try {
                CheckMsg(msg_wrapper);

                ibmw::transport::SubscriberReleaseCallback release_callback(
                    [release_callback_shared_ptr, msg_cache_ptr{msg_wrapper.msg_cache_ptr}]() {});

                (*sub_func_ptr)(
                    msg_wrapper.ctx_ref.NativeHandle(),
                    msg_wrapper.msg_ptr,
                    release_callback.NativeHandle());

              } catch (const std::exception& e) {
                MW_ERROR("{}", e.what());
              }
            },
            msg_wrapper);
      };

  // register sub wrapper
  const auto& msg_handler_ref = *msg_handler_ptr;

  if (!transport_catalog_ptr_->OnMessage(std::move(msg_handler_ptr)))
    return false;

  auto driver_itr = sub_topics_driver_index_map_.find(topic_name);
  if (driver_itr == sub_topics_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(topic_name, sub_topics_drivers_rules_);
    auto emplace_ret = sub_topics_driver_index_map_.emplace(topic_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Subscribe type '{}' for topic '{}' to driver '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->OnMessage(msg_handler_ref);
  }
  return ret;
}

bool TransportDriverDispatcher::RegisterSendType(RegisterSendTypeAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Publish type can only be registered when state is 'Init'.");
    return false;
  }

  if (wrapper.msg_type_support == nullptr) [[unlikely]] {
    MW_ERROR("Msg type support is null.");
    return false;
  }

  auto topic_name = wrapper.topic_name;
  auto msg_type_support_ref = ibmw::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();

  // create pub wrapper
  auto send_type_wrapper_ptr = std::make_unique<SendTypeWrapper>();
  send_type_wrapper_ptr->info = TransportInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++pub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};

  // initialize publish sequence for this topic (only in Init state)
  auto seq_emplace = pub_topic_seq_map_.try_emplace(std::string(topic_name), 0);
  std::atomic<uint32_t>* seq_counter_ptr = &seq_emplace.first->second;

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, publish_filters_rules_);
  publish_filter_bus_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);
  const auto* filter_collector_ptr =
      &publish_filter_bus_ptr_->GetFilterCollector(topic_name);

  // resolve drivers for this topic (Phase 7 §7.2 C2: do this before
  // moving wrapper into catalog so we can populate the cached_drivers
  // field on the wrapper itself; the dispatcher's own
  // pub_topics_driver_index_map_ is also kept for legacy lookups but
  // hot-path Send() will read directly from SendTypeWrapper.cached_drivers).
  auto driver_itr = pub_topics_driver_index_map_.find(topic_name);
  if (driver_itr == pub_topics_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(topic_name, pub_topics_drivers_rules_);
    auto emplace_ret = pub_topics_driver_index_map_.emplace(topic_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  // Phase 7 §7.2 C2: populate cached pointers before ownership transfer.
  // Driver vector is copied by value so it survives any later rehash of
  // pub_topics_driver_index_map_.
  send_type_wrapper_ptr->cached_drivers = driver_itr->second;
  send_type_wrapper_ptr->cached_seq_counter = seq_counter_ptr;
  send_type_wrapper_ptr->cached_filter_collector = filter_collector_ptr;

  // register pub wrapper
  const auto& send_type_wrapper_ref = *send_type_wrapper_ptr;

  if (!transport_catalog_ptr_->RegisterSendType(std::move(send_type_wrapper_ptr)))
    return false;

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Register publish type '{}' for topic '{}' to driver '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->RegisterSendType(send_type_wrapper_ref);
  }
  return ret;
}

void TransportDriverDispatcher::Send(SendAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kStart) [[unlikely]] {
    MW_WARN("Method can only be called when state is 'Start'.");
    return;
  }

  auto msg_type = util::ToStdStringView(wrapper.msg_type);

  ibmw::transport::Context default_ctx;
  ibmw::transport::ContextRef ctx_ref(
      wrapper.ctx_ptr ? wrapper.ctx_ptr : default_ctx.NativeHandle());

  // Find the registered publisher type
  const auto* send_type_wrapper_ptr = transport_catalog_ptr_->GetSendTypeWrapperPtr(
      msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);

  // If publish type is not registered
  if (send_type_wrapper_ptr == nullptr) {
    MW_WARN(
        "Publish type unregistered, msg_type: {}, topic_name: {}, pkg_path: {}, module_name: {}.",
        msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);
    return;
  }

  // Check ctx
  if (ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT ||
      ctx_ref.CheckUsed()) {
    MW_WARN("Publish context has been used!");
    return;
  }

  ctx_ref.SetUsed();

  // set publish sequence into context (Phase 7 §7.2 C2: cached counter
  // pointer skips per-message map lookup; pointer was populated at
  // RegisterSendType time and is non-null for any wrapper produced by
  // RegisterSendType).
  if (auto* seq_counter = send_type_wrapper_ptr->cached_seq_counter) [[likely]] {
    uint32_t seq = ++(*seq_counter);
    SetMetaUint64(ctx_ref, IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ, seq);
  }
  SetMetaUint64(ctx_ref, IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP,
                ibmw::common::util::GetCurTimestampNs());

  // Phase 7 §7.2 C2: cached filter collector + driver vector skip two
  // additional per-message hash lookups (filter_bus and
  // pub_topics_driver_index_map_).
  const auto& filter_collector = *send_type_wrapper_ptr->cached_filter_collector;
  const auto& cached_drivers = send_type_wrapper_ptr->cached_drivers;

  // Phase 7 §7.2 C4: stack-allocated MessageFrame. InvokeTransport is
  // strictly synchronous (transport_framework_async_filter.h:40-42 calls
  // final_filter_(msg, h) inline) and IceoryxTransportDriver::Send copies
  // payload into shm and returns without retaining ownership, so the
  // frame is safe to live on the caller stack — saves 1 heap allocation
  // (control block + frame) per message.
  MessageFrame frame{
      .info = send_type_wrapper_ptr->info,
      .msg_ptr = wrapper.msg_ptr,
      .ctx_ref = ctx_ref};

  // Start publish
  filter_collector.InvokeTransport(
      [this, &cached_drivers](MessageFrame& msg_wrapper) {
        for (auto* itr : cached_drivers) {
          MW_TRACE("Publish msg '{}' for topic '{}' to transport driver '{}'",
                      msg_wrapper.info.msg_type, msg_wrapper.info.topic_name, itr->Name());
          itr->Send(msg_wrapper);
        }
      },
      frame);
}

bool TransportDriverDispatcher::OnMessage(MessageHandlerWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Msg can only be subscribed when state is 'Init'.");
    return false;
  }

  auto msg_handler_ptr = std::make_unique<MessageHandlerWrapper>(std::move(wrapper));

  std::string_view topic_name = msg_handler_ptr->info.topic_name;
  std::string_view msg_type = msg_handler_ptr->info.msg_type;

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, subscribe_filters_rules_);
  subscribe_filter_bus_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);

  // set callback
  const auto& filter_collector = subscribe_filter_bus_ptr_->GetFilterCollector(topic_name);

  msg_handler_ptr->callback =
      [&filter_collector, callback{std::move(msg_handler_ptr->callback)}](
          MessageFrame& msg_wrapper, std::function<void()>&& input_release_callback) {
        auto release_callback_shared_ptr = std::shared_ptr<std::function<void()>>(
            new std::function<void()>(std::move(input_release_callback)),
            [](std::function<void()>* f) {
              (*f)();
              delete f;
            });

        filter_collector.InvokeTransport(
            [&callback, release_callback_shared_ptr](MessageFrame& msg_wrapper) {
              callback(
                  msg_wrapper,
                  [release_callback_shared_ptr, msg_cache_ptr{msg_wrapper.msg_cache_ptr}]() {});
            },
            msg_wrapper);
      };

  // register sub wrapper
  const auto& msg_handler_ref = *msg_handler_ptr;

  if (!transport_catalog_ptr_->OnMessage(std::move(msg_handler_ptr)))
    return false;

  auto driver_itr = sub_topics_driver_index_map_.find(topic_name);
  if (driver_itr == sub_topics_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(topic_name, sub_topics_drivers_rules_);
    auto emplace_ret = sub_topics_driver_index_map_.emplace(topic_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Subscribe type '{}' for topic '{}' to driver '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->OnMessage(msg_handler_ref);
  }
  return ret;
}

bool TransportDriverDispatcher::RegisterSendType(SendTypeWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Publish type can only be registered when state is 'Init'.");
    return false;
  }

  auto send_type_wrapper_ptr = std::make_unique<SendTypeWrapper>(std::move(wrapper));

  std::string_view topic_name = send_type_wrapper_ptr->info.topic_name;
  std::string_view msg_type = send_type_wrapper_ptr->info.msg_type;

  // initialize publish sequence for this topic (only in Init state)
  auto seq_emplace = pub_topic_seq_map_.try_emplace(std::string(topic_name), 0);
  std::atomic<uint32_t>* seq_counter_ptr = &seq_emplace.first->second;

  // create filter
  auto filter_name_vec = GetFilterRules(topic_name, publish_filters_rules_);
  publish_filter_bus_ptr_->CreateFilterCollectorIfNotExist(topic_name, filter_name_vec);
  const auto* filter_collector_ptr =
      &publish_filter_bus_ptr_->GetFilterCollector(topic_name);

  // resolve drivers (Phase 7 §7.2 C2: same as the access overload).
  auto driver_itr = pub_topics_driver_index_map_.find(topic_name);
  if (driver_itr == pub_topics_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(topic_name, pub_topics_drivers_rules_);
    auto emplace_ret = pub_topics_driver_index_map_.emplace(topic_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  // populate cached pointers before catalog takes ownership
  send_type_wrapper_ptr->cached_drivers = driver_itr->second;
  send_type_wrapper_ptr->cached_seq_counter = seq_counter_ptr;
  send_type_wrapper_ptr->cached_filter_collector = filter_collector_ptr;

  // register pub wrapper
  const auto& send_type_wrapper_ref = *send_type_wrapper_ptr;

  if (!transport_catalog_ptr_->RegisterSendType(std::move(send_type_wrapper_ptr)))
    return false;

  bool ret = true;
  for (auto& itr : driver_itr->second) {
    MW_TRACE("Register publish type '{}' for topic '{}' to driver '{}'.",
                msg_type, topic_name, itr->Name());
    ret &= itr->RegisterSendType(send_type_wrapper_ref);
  }
  return ret;
}

void TransportDriverDispatcher::Send(MessageFrame&& wrapper) {
  if (state_.load() != State::kStart) [[unlikely]] {
    MW_WARN("Method can only be called when state is 'Start'.");
    return;
  }

  auto publish_msg_wrapper_ptr = std::make_shared<MessageFrame>(std::move(wrapper));

  auto ctx_ref = publish_msg_wrapper_ptr->ctx_ref;

  // Check ctx
  if (ctx_ref.GetType() != ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT ||
      ctx_ref.CheckUsed()) {
    MW_WARN("Publish context has been used!");
    return;
  }

  ctx_ref.SetUsed();

  // set publish sequence into context if available
  auto it = pub_topic_seq_map_.find(publish_msg_wrapper_ptr->info.topic_name);
  if (it != pub_topic_seq_map_.end()) {
    uint32_t seq = ++(it->second);
    SetMetaUint64(ctx_ref, IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ, seq);
  }

  SetMetaUint64(ctx_ref, IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP,
                ibmw::common::util::GetCurTimestampNs());

  // Find filter
  const auto& filter_collector =
      publish_filter_bus_ptr_->GetFilterCollector(publish_msg_wrapper_ptr->info.topic_name);

  // Start publish
  filter_collector.InvokeTransport(
      [this](MessageFrame& msg_wrapper) {
        std::string_view topic_name = msg_wrapper.info.topic_name;

        auto find_itr = pub_topics_driver_index_map_.find(topic_name);

        if (find_itr == pub_topics_driver_index_map_.end()) [[unlikely]] {
          MW_WARN("Transport msg has no driver, topic: '{}'.", topic_name);
          return;
        }

        for (auto& itr : find_itr->second) {
          MW_TRACE("Publish msg '{}' for topic '{}' to transport driver '{}'",
                      msg_wrapper.info.msg_type, topic_name, itr->Name());
          itr->Send(msg_wrapper);
        }
      },
      *publish_msg_wrapper_ptr);
}

// Phase 7 §7.3 §K-3.3: dispatcher-level user Loan/PublishLoaned/ReleaseLoaned.
//
// Design notes
// ------------
// * Loan() reuses the SendTypeWrapper looked up by the same key that
//   Send() uses, so an application that has called RegisterSendType()
//   for a topic can immediately Loan() against it. The hot path skips
//   ctx-meta updates (pub_seq / pub_ts) and the publish filter chain
//   intentionally — this is the explicit zero-copy fast lane (§7.3 §F).
// * The first driver in cached_drivers whose LoanSample() returns true
//   wins. Multi-driver fan-out for loaned samples is not meaningful
//   (each driver would need its own loan), so a single source_driver is
//   recorded on the handle and PublishLoaned/ReleaseLoaned route back
//   to it via that field.
// * On any failure we return an empty handle (user_payload_ptr ==
//   nullptr, source_driver == nullptr) — callers must check.
LoanedSampleHandle TransportDriverDispatcher::Loan(LoanAccessInfoWrapper&& wrapper) {
  LoanedSampleHandle handle{};

  if (state_.load() != State::kStart) [[unlikely]] {
    MW_WARN("Loan can only be called when state is 'Start'.");
    return handle;
  }

  auto msg_type = util::ToStdStringView(wrapper.msg_type);

  const auto* send_type_wrapper_ptr = transport_catalog_ptr_->GetSendTypeWrapperPtr(
      msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);

  if (send_type_wrapper_ptr == nullptr) [[unlikely]] {
    MW_WARN(
        "Loan publish type unregistered, msg_type: {}, topic_name: {}, pkg_path: {}, module_name: {}.",
        msg_type, wrapper.topic_name, wrapper.pkg_path, wrapper.module_name);
    return handle;
  }

  // Try drivers in registration order; first one that supports loan for
  // this (topic, mode) wins. Driver is responsible for setting
  // out_handle.source_driver = this on success (see
  // loaned_sample_handle.h doc comment).
  for (auto* driver : send_type_wrapper_ptr->cached_drivers) {
    if (driver->LoanSample(send_type_wrapper_ptr->info, wrapper.requested_size,
                           wrapper.mode, /*ctx=*/nullptr, handle)) {
      return handle;
    }
  }

  // No driver could fulfill the loan; handle is still zeroed.
  return handle;
}

void TransportDriverDispatcher::PublishLoaned(LoanedSampleHandle& handle) {
  if (state_.load() != State::kStart) [[unlikely]] {
    MW_WARN("PublishLoaned can only be called when state is 'Start'.");
    return;
  }

  if (handle.source_driver == nullptr) [[unlikely]] {
    MW_WARN("PublishLoaned called on empty/already-consumed handle.");
    return;
  }

  // Driver is contracted to clear source_driver (and other fields it
  // chooses) on completion, so the handle becomes safe to drop.
  handle.source_driver->PublishLoaned(handle);
}

void TransportDriverDispatcher::ReleaseLoaned(LoanedSampleHandle& handle) {
  // Release tolerates Shutdown so RAII wrappers finishing teardown
  // don't spam warnings; only PreInit/Init are clearly wrong.
  auto st = state_.load();
  if (st == State::kPreInit || st == State::kInit) [[unlikely]] {
    MW_WARN("ReleaseLoaned called before Start.");
    return;
  }

  if (handle.source_driver == nullptr) {
    return;  // already published or never loaned; silently no-op.
  }

  handle.source_driver->ReleaseLoaned(handle);
}

// Phase 7 §7.3 §U: receiver-side zero-copy fast lane.
//
// Mirrors Loan() above: deliberately bypasses the catalog, the
// subscribe filter chain and the MessageFrame deserialization path so
// the user callback receives the driver's borrowed payload pointer
// directly (typically a SHM chunk payload). Drivers are tried in
// registration order via sub_topics_driver_index_map_; first one that
// returns true wins. Returns false if no driver supports zero-copy
// receive for this topic+type — caller (LoanedSubscriber<T>) is
// expected to fall back to OnMessage.
bool TransportDriverDispatcher::AttachLoaned(MessageHandlerAccessInfoWrapper&& wrapper) {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("Loaned msg can only be subscribed when state is 'Init'.");
    return false;
  }

  if (wrapper.msg_type_support == nullptr) [[unlikely]] {
    MW_ERROR("Msg type support is null.");
    return false;
  }

  if (wrapper.callback == nullptr) [[unlikely]] {
    MW_ERROR("Callback is null.");
    return false;
  }

  auto topic_name = wrapper.topic_name;
  auto msg_type_support_ref = ibmw::util::TypeSupportRef(wrapper.msg_type_support);
  auto msg_type = msg_type_support_ref.TypeName();

  // Build the loaned handler wrapper directly (no catalog registration —
  // loan path bypasses filter chain and MessageFrame).
  LoanedMessageHandlerWrapper loaned_handler;
  loaned_handler.info = TransportInfo{
      .msg_type = std::string(msg_type),
      .topic_name = std::string(topic_name),
      .pkg_path = std::string(wrapper.pkg_path),
      .module_name = std::string(wrapper.module_name),
      .index = ++sub_topic_index_,
      .msg_type_support_ref = msg_type_support_ref};

  auto sub_func_shared_ptr = std::make_shared<ibmw::transport::LoanedMessageCallback>(wrapper.callback);

  loaned_handler.callback =
      [this, sub_func_shared_ptr](
          const ibmw_transport_context_base_t* ctx_ptr,
          const void* payload_ptr,
          size_t payload_size,
          std::function<void()>&& input_release_callback) {
        // §V O4: stack-allocate the release callback wrapper so the
        // hot path does NOT allocate per chunk. The previous
        // implementation built a shared_ptr<std::function<void()>>
        // with a custom deleter that re-invoked the release functor
        // — that meant new + shared_ptr ctrl block + delete every
        // single delivery. The SHM PodMessageCallback path passes
        // a no-op release functor (driver releases the chunk
        // synchronously inside its take() lambda), so we just hold
        // input_release_callback by value on the stack and let the
        // SubscriberReleaseCallback wrapper invoke it on scope exit
        // if non-empty. Drivers that need async release MUST move
        // input_release_callback into their own storage before
        // invoking this callback (no current driver does so).
        try {
          ibmw::transport::SubscriberReleaseCallback release_callback(
              [&input_release_callback]() {
                if (input_release_callback) input_release_callback();
              });

          (*sub_func_shared_ptr)(
              ctx_ptr,
              payload_ptr,
              payload_size,
              release_callback.NativeHandle());

        } catch (const std::exception& e) {
          MW_ERROR("{}", e.what());
        }
      };

  // Resolve sub-topic driver list (lazy populate, same pattern as
  // OnMessage above).
  auto driver_itr = sub_topics_driver_index_map_.find(topic_name);
  if (driver_itr == sub_topics_driver_index_map_.end()) {
    auto driver_ptr_vec = GetDriversByRules(topic_name, sub_topics_drivers_rules_);
    auto emplace_ret = sub_topics_driver_index_map_.emplace(topic_name, std::move(driver_ptr_vec));
    driver_itr = emplace_ret.first;
  }

  // First-success-wins: ask each driver in registration order.
  for (auto* driver : driver_itr->second) {
    MW_TRACE("AttachLoaned type '{}' for topic '{}' to driver '{}'.",
                msg_type, topic_name, driver->Name());
    if (driver->AttachLoaned(loaned_handler)) {
      return true;
    }
  }

  MW_WARN(
      "No driver supports zero-copy receive for topic '{}' (msg_type '{}'). "
      "LoanedSubscriber callers should fall back to OnMessage.",
      topic_name, msg_type);
  return false;
}

}  // namespace ibmw::runtime::core::transport
