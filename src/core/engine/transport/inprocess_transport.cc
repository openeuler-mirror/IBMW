// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/inprocess_transport.h"
#include "engine/transport/transport_driver_tools.h"

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace YAML {
template <>
struct convert<ibmw::runtime::core::transport::InProcessTransport::Options> {
  using Options = ibmw::runtime::core::transport::InProcessTransport::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["subscriber_use_inline_executor"] = rhs.subscriber_use_inline_executor;
    if (!rhs.subscriber_use_inline_executor)
      node["subscriber_executor"] = rhs.subscriber_executor;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    rhs.subscriber_use_inline_executor = node["subscriber_use_inline_executor"].as<bool>();
    if (!rhs.subscriber_use_inline_executor)
      rhs.subscriber_executor = node["subscriber_executor"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::transport {

namespace {

// Copying a message with the subscriber package's type support is only sound
// when both packages provably share the implementation; msg_type names alone
// do not guarantee an identical layout (COMM-007).
bool SameCopyImplementation(const ibmw::util::TypeSupportRef& pub, const ibmw::util::TypeSupportRef& sub) {
  const auto* p = pub.NativeHandle();
  const auto* s = sub.NativeHandle();
  if (p == s) return true;
  if (p == nullptr || s == nullptr) return false;
  if (p->copy == s->copy && p->ctx == s->ctx) return true;
  if (p->custom_type_support_ptr == nullptr || s->custom_type_support_ptr == nullptr) return false;
  const void* p_custom = p->custom_type_support_ptr(p->ctx);
  return p_custom != nullptr && p_custom == s->custom_type_support_ptr(s->ctx);
}

constexpr auto kShutdownDrainTimeout = std::chrono::seconds(5);

}  // namespace

void InProcessTransport::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Local transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  if (!options_.subscriber_use_inline_executor) {
    MW_REQUIRE(
        get_executor_func_,
        "Get executor function is not set before initialize.");

    subscribe_executor_ref_ = get_executor_func_(options_.subscriber_executor);

    MW_REQUIRE(
        subscribe_executor_ref_,
        "Invalid local subscriber executor '{}'.", options_.subscriber_executor);
  }

  options_node = options_;
}

void InProcessTransport::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

InProcessTransport::~InProcessTransport() {
  if (inflight_->value.load(std::memory_order_acquire) == 0) {
    delete inflight_;
  }
  // Otherwise discarded delivery tasks may still release their token: the
  // small counter block is deliberately leaked (already reported by
  // DrainInflight()).
}

void InProcessTransport::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  if (subscribe_executor_ref_) DrainInflight();

  // subscribe_index_map_ is immutable after Start() and is read by Send()
  // before it takes its in-flight token, so it is only released with the
  // driver itself.

  get_executor_func_ = nullptr;
}

void InProcessTransport::DrainInflight() {
  if (subscribe_executor_ref_.IsInCurrentExecutor()) {
    MW_WARN("Local transport shut down from its subscriber executor; queued "
            "deliveries are not drained.");
    return;
  }
  // Pairs with the seq_cst increment in Send(): either Send() observes
  // kShutdown, or this load observes its increment.
  if (inflight_->value.load(std::memory_order_seq_cst) == 0) return;
  const auto deadline = std::chrono::steady_clock::now() + kShutdownDrainTimeout;
  while (inflight_->value.load(std::memory_order_acquire) != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      MW_ERROR("Local transport shutdown timed out with {} delivery(ies) "
               "still pending on executor '{}'.",
               inflight_->value.load(std::memory_order_relaxed), options_.subscriber_executor);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void InProcessTransport::ReportIncompatibleCopy(
    const TransportInfo& pub_info, std::string_view sub_pkg_path) noexcept {
  try {
    std::string key = pub_info.topic_name + '\n' + pub_info.pkg_path + '\n' + std::string(sub_pkg_path);
    {
      std::lock_guard lock(incompatible_copy_mutex_);
      if (!incompatible_copy_reported_.insert(std::move(key)).second) return;
    }
    MW_ERROR(
        "Local transport drops '{}' ({}) from pkg '{}' to pkg '{}': the type "
        "supports differ and support no serialization, so a direct copy is "
        "not known to be layout-compatible.",
        pub_info.topic_name, pub_info.msg_type, pub_info.pkg_path, sub_pkg_path);
  } catch (...) {
  }
}

bool InProcessTransport::RegisterSendType(
    const SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Publish type can only be registered when state is 'Init'.");
      return false;
    }

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool InProcessTransport::OnMessage(const MessageHandlerWrapper& message_handler) noexcept {
  try {
    if (state_.load() != State::kInit) {
      MW_ERROR("Msg can only be subscribed when state is 'Init'.");
      return false;
    }

    const auto& info = message_handler.info;

    subscribe_index_map_[info.msg_type][info.topic_name][info.pkg_path].emplace(info.module_name);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void InProcessTransport::Send(MessageFrame& msg_wrapper) noexcept {
  try {
    if (state_.load() != State::kStart) [[unlikely]] {
      MW_WARN("Method can only be called when state is 'Start'.");
      return;
    }

    const auto& pub_info = msg_wrapper.info;

    // Return immediately if there are no subscribers
    auto subscribe_index_map_find_msg_itr = subscribe_index_map_.find(pub_info.msg_type);
    if (subscribe_index_map_find_msg_itr == subscribe_index_map_.end()) [[unlikely]]
      return;

    auto subscribe_index_map_find_topic_itr = subscribe_index_map_find_msg_itr->second.find(pub_info.topic_name);
    if (subscribe_index_map_find_topic_itr == subscribe_index_map_find_msg_itr->second.end()) [[unlikely]]
      return;

    // Only deliveries need the in-flight token (catalog handlers, executor
    // tasks), so publishing without subscribers stays free of it. The
    // seq_cst increment followed by the state re-check pairs with the
    // exchange + load in Shutdown()/DrainInflight().
    std::optional<InflightToken> send_token;
    if (subscribe_executor_ref_) {
      send_token.emplace(*inflight_, std::memory_order_seq_cst);
      if (state_.load() != State::kStart) [[unlikely]]
        return;
    }

    // Determine the serialization type
    auto serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty() &&
        pub_info.msg_type_support_ref.SerializationTypesSupportedNum() > 0)
      serialization_type = pub_info.msg_type_support_ref.DefaultSerializationType();

    // Count how many packages will use serialization-based delivery so we can
    // move the serialization_cache to the very last serialization subscriber
    // instead of copying it N times.
    const bool type_supports_serialization =
        pub_info.msg_type_support_ref.SerializationTypesSupportedNum() > 0;
    size_t serialization_pkg_remaining = 0;
    if (type_supports_serialization) {
      for (const auto& sp : subscribe_index_map_find_topic_itr->second) {
        if (sp.first != pub_info.pkg_path)
          ++serialization_pkg_remaining;
      }
    }

    // Iterate through each pkg
    for (const auto& subscribe_pkg_path_itr : subscribe_index_map_find_topic_itr->second) {
      std::string_view cur_sub_pkg_path = subscribe_pkg_path_itr.first;
      // One package failing (e.g. its serialization throws) must not drop the
      // delivery to the packages after it.
      try {
        const auto* module_msg_handler_map_ptr = transport_catalog_ptr_->GetModuleMessageHandlerWrapperMapPtr(
            pub_info.msg_type, pub_info.topic_name, cur_sub_pkg_path);

        if (module_msg_handler_map_ptr == nullptr ||
            module_msg_handler_map_ptr->empty()) [[unlikely]] {
          MW_ERROR(
              "Can not find catalog info, pkg_path: {}, topic_name: {}, msg_type: {}",
              cur_sub_pkg_path, pub_info.topic_name, pub_info.msg_type);
          continue;
        }

        // The creation/destruction methods of the same type should be unified for each module under the same pkg.
        // Select any module's creation/destruction method for this type as the global selection of pkg
        const auto* tpl_msg_handler_ptr = module_msg_handler_map_ptr->begin()->second;

        auto tpl_sub_info = tpl_msg_handler_ptr->info;

        // Create context under this pkg.
        // NOTE: The context is shared across all module subscribers within the
        // same package. This is safe for sequential (inline) execution, but when
        // subscribe_executor_ref_ is set, concurrent callbacks may race on
        // Context mutation if a subscriber filter (e.g. otp_trace) calls
        // SetMetaValue. This is a pre-existing design trade-off; adding per-
        // subscriber Context clones would eliminate the race at the cost of
        // additional heap allocations.
        auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

        const auto& meta_keys = msg_wrapper.ctx_ref.GetMetaKeys();
        for (const auto& item : meta_keys) {
          ctx_ptr->SetMetaValue(item, msg_wrapper.ctx_ref.GetMetaValue(item));
        }

        ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

        // Determine whether to use copy or serialization for message delivery.
        // Use copy when: (a) pub and sub are in the same package, OR
        // (b) the type support does not support serialization. Case (b) is
        // only sound when both packages share the copy implementation;
        // otherwise the message is dropped rather than copied with a possibly
        // different layout.
        const bool same_pkg = (cur_sub_pkg_path == pub_info.pkg_path);
        bool use_copy = same_pkg || !type_supports_serialization;
        if (!same_pkg && use_copy &&
            !SameCopyImplementation(pub_info.msg_type_support_ref, tpl_sub_info.msg_type_support_ref)) [[unlikely]] {
          ReportIncompatibleCopy(pub_info, cur_sub_pkg_path);
          continue;
        }

        if (use_copy) {
          // Copy-based delivery: create a new message and copy data directly.
          // Only ONE copy per subscribing package — all modules within the same
          // package share the same message via shared_ptr.

          // Create sub msg under this pkg (single allocation for all subscribers)
          std::shared_ptr<void> msg_ptr = tpl_sub_info.msg_type_support_ref.CreateSharedPtr();

          CheckMsg(msg_wrapper);

          tpl_sub_info.msg_type_support_ref.Copy(msg_wrapper.msg_ptr, msg_ptr.get());

          // Call the registered subscribe method.
          // Note: serialization_cache is intentionally omitted — on the copy
          // path the subscriber already has a direct msg_ptr and msg_cache_ptr,
          // so the cache is never consulted by CheckMsg() or downstream code.
          for (const auto& msg_handler_itr : *module_msg_handler_map_ptr) {
            const auto* msg_handler_ptr = msg_handler_itr.second;

            // Create MessageFrame under this pkg-module
            MessageFrame sub_msg_wrapper{
                .info = msg_handler_ptr->info,
                .msg_ptr = msg_ptr.get(),
                .ctx_ref = ctx_ptr,
                .msg_cache_ptr = msg_ptr};

            if (subscribe_executor_ref_) {
              subscribe_executor_ref_.Dispatch(
                  [msg_handler_ptr, ctx_ptr, sub_msg_wrapper{std::move(sub_msg_wrapper)},
                   token = InflightToken(*inflight_, std::memory_order_relaxed)]() mutable {
                    msg_handler_ptr->callback(sub_msg_wrapper, [ctx_ptr]() {}, nullptr);
                  });
            } else {
              msg_handler_ptr->callback(sub_msg_wrapper, [ctx_ptr]() {}, nullptr);
            }
          }

        } else {
          // Serialization-based delivery: when pub and sub are in different
          // packages and the type support provides serialization methods.

          ctx_ptr->SetSerializationType(serialization_type);

          // Serialize message on publisher side
          SerializeMsgWithCache(msg_wrapper, serialization_type);

          --serialization_pkg_remaining;
          const bool is_last_serialization_pkg = (serialization_pkg_remaining == 0);

          // Call the registered subscribe method.
          // Optimization: move the serialization_cache to the last subscriber of
          // the last serialization-package instead of copying for all — saves
          // (total_subscribers - 1) unordered_map copy operations.
          const size_t handler_count = module_msg_handler_map_ptr->size();
          size_t handler_idx = 0;

          for (const auto& msg_handler_itr : *module_msg_handler_map_ptr) {
            const auto* msg_handler_ptr = msg_handler_itr.second;
            ++handler_idx;
            const bool is_last = is_last_serialization_pkg && (handler_idx == handler_count);

            // Create MessageFrame under this pkg-module
            MessageFrame sub_msg_wrapper{
                .info = msg_handler_ptr->info,
                .msg_ptr = nullptr,
                .ctx_ref = ctx_ptr,
                .serialization_cache = is_last
                    ? std::move(msg_wrapper.serialization_cache)
                    : msg_wrapper.serialization_cache};

            if (subscribe_executor_ref_) {
              subscribe_executor_ref_.Dispatch(
                  [msg_handler_ptr, ctx_ptr, sub_msg_wrapper{std::move(sub_msg_wrapper)},
                   token = InflightToken(*inflight_, std::memory_order_relaxed)]() mutable {
                    msg_handler_ptr->callback(sub_msg_wrapper, [ctx_ptr]() {}, nullptr);
                  });
            } else {
              msg_handler_ptr->callback(sub_msg_wrapper, [ctx_ptr]() {}, nullptr);
              // Inline delivery is over: hand the cache back so the drivers
              // after this one do not serialize again.
              if (is_last) msg_wrapper.serialization_cache = std::move(sub_msg_wrapper.serialization_cache);
            }
          }
        }
      } catch (const std::exception& e) {
        MW_ERROR("Deliver msg to pkg '{}' failed, topic_name: {}, msg_type: {}, error: {}",
                 cur_sub_pkg_path, pub_info.topic_name, pub_info.msg_type, e.what());
      }
    }
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void InProcessTransport::RegisterGetExecutorFunc(
    const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_executor_func_ = get_executor_func;
}

}  // namespace ibmw::runtime::core::transport
