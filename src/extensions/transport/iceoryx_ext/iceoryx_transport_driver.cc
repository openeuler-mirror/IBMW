// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "cpp/util/buffer_array_allocator.h"
#include "iceoryx_ext/global.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "iceoryx_posh/mepoo/chunk_header.hpp"
#include <limits>

namespace YAML {
template <>
struct convert<ibmw::extensions::iceoryx_extension::IceoryxTransportDriver::Options> {
  using Options = ibmw::extensions::iceoryx_extension::IceoryxTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["listener_thread_name"] = rhs.listener_thread_name;
    node["listener_thread_sched_policy"] = rhs.listener_thread_sched_policy;
    node["listener_thread_bind_cpu"] = rhs.listener_thread_bind_cpu;

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

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::iceoryx_extension {
void IceoryxTransportDriver::Initialize(YAML::Node options_node) {
  // todo: check options_node->shm_init_size

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Iceoryx transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull()) {
    MW_REQUIRE(
        !options_node["executor"] && !options_node["callback_executor"] &&
            !options_node["subscribers"],
        "Iceoryx transport driver does not support callback executor options; "
        "use listener_thread_* options or the Context::CreateSubscriber(..., executor, cb) API-layer binding.");
    options_ = options_node.as<Options>();
  }

  options_node = options_;
}

void IceoryxTransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
}

void IceoryxTransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  iceoryx_bus_.Shutdown();
}

bool IceoryxTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");
    namespace util = ibmw::common::util;

    const auto& info = send_type_wrapper.info;
    std::string pattern = std::string("/transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type);

    // register publisher with url to iceoryx
    iceoryx_bus_.RegisterPublisher(pattern);

    MW_INFO("Register publish type to iceoryx transport, url: {}", pattern);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool IceoryxTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

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
    auto msg_handler_tool_unique_ptr = std::make_unique<runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    message_handler_map_.emplace(pattern, std::move(msg_handler_tool_unique_ptr));

    auto handle =
        [this, topic_name = info.topic_name, msg_handler_tool_ptr](iox::popo::UntypedSubscriber* subscriber) {
          try {
            // if not set sched info, set it
            if (!sched_info_set_) [[unlikely]] {
              sched_info_set_ = true;
              auto thread_name = options_.listener_thread_name;
              if (!thread_name.empty()) {
                runtime::core::util::SetNameForCurrentThread(thread_name);
              }

              auto cpu_set = options_.listener_thread_bind_cpu;
              if (!cpu_set.empty()) {
                runtime::core::util::BindCpuForCurrentThread(cpu_set);
              }

              auto sched_policy = options_.listener_thread_sched_policy;
              if (!sched_policy.empty()) {
                runtime::core::util::SetCpuSchedForCurrentThread(sched_policy);
              }
            }
            // read data from shared memory : pkg_size | serialization_type | ctx_num | ctx_key1 | ctx_val1 | ... | ctx_keyN | ctx_valN | msg_buffer
            // use while struck to make sure all packages are read
            while (subscriber->hasData()) {
              subscriber->take()
                  .and_then([&](const void* payload) {
                    auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

                    uint32_t pkg_size = util::GetUint32FromBuf(static_cast<const char*>(payload));

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

                    ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

                    // get msg buffer
                    auto remaining_buf = buf_oper.GetRemainingBuffer();

                    msg_handler_tool_ptr->DoMessageCallback(
                        ctx_ptr, serialization_type, static_cast<const void*>(remaining_buf.data()), remaining_buf.size());

                    // release shm
                    subscriber->release(payload);
                  })
                  .or_else([](auto& error) {
                    ;  // data has not been ready
                  });
            }
          } catch (const std::exception& e) {
            MW_WARN("Handle Iceoryx transport msg failed, exception info: {}", e.what());
          }
        };

    iceoryx_bus_.RegisterSubscriber(pattern, std::move(handle));

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

    namespace util = ibmw::common::util;

    const auto& info = msg_wrapper.info;

    std::string iceoryx_pub_topic = std::string("/transport/") +
                                    util::UrlEncode(info.topic_name) + "/" +
                                    util::UrlEncode(info.msg_type);

    // find publisher
    auto* iox_pub_ctx_ptr = iceoryx_bus_.GetPublisher(iceoryx_pub_topic);
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
    if (finditr != serialization_cache.end()) [[unlikely]] {
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
          iox_pub_ctx_ptr->UpdateLoanShm(loaned_shm, loaned_shm.Size() * 2);
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

// Bookkeeping kept alive across LoanSampleWithCtx -> PublishLoaned/ReleaseLoaned.
// Allocated on borrow, freed on publish/release. Pointed to by
// LoanedSampleHandle::driver_token. We never put this pointer into POD-mode
// handles (POD mode stores IoxPublisher* directly to keep that path lean).
struct CtxMetaLoanToken {
  ibmw::extensions::iceoryx_extension::IoxPublisher* pub;
  void* chunk_ptr;     // start of iceoryx user payload (i.e. base of header)
  size_t total_size;   // header_size + payload_size, for pkg_size field
};
}  // namespace

bool IceoryxTransportDriver::EnsurePodTopic(std::string_view topic,
                                            std::string_view msg_type,
                                            size_t /*initial_size_hint*/) noexcept {
  try {
    auto key = MakePodKey(topic, msg_type);
    if (pod_pub_map_.find(key) != pod_pub_map_.end()) return true;

    auto url = MakePodUrl(topic, msg_type);
    iceoryx_bus_.RegisterPublisher(url);
    auto* pub = iceoryx_bus_.GetPublisher(url);
    if (pub == nullptr) {
      MW_ERROR("EnsurePodTopic: failed to obtain publisher for url={}", url);
      return false;
    }
    pod_pub_map_.emplace(std::move(key), pub);
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
  try {
    auto url = MakePodUrl(topic, msg_type);

    auto cb_storage = std::make_unique<PodMessageCallback>(std::move(callback));
    auto* cb_ptr = cb_storage.get();
    pod_callback_storage_.push_back(std::move(cb_storage));

    auto handle = [cb_ptr](iox::popo::UntypedSubscriber* subscriber) {
      while (subscriber->hasData()) {
        subscriber->take()
            .and_then([&](const void* payload) {
              auto chunk_header =
                  iox::mepoo::ChunkHeader::fromUserPayload(payload);
              size_t payload_size =
                  chunk_header ? chunk_header->userPayloadSize() : 0;
              try {
                (*cb_ptr)(payload, payload_size);
              } catch (const std::exception& e) {
                MW_WARN("POD subscriber callback threw: {}", e.what());
              }
              subscriber->release(payload);
            })
            .or_else([](auto&) { /* no data ready */ });
      }
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
// wrapped in a PodMessageCallback that drops ctx_ptr (POD path has
// none) and the release functor (iceoryx releases the chunk
// automatically when our internal handler returns).
bool IceoryxTransportDriver::AttachLoaned(
    const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler) noexcept {
  if (state_.load() != State::kInit) [[unlikely]] {
    MW_ERROR("AttachLoaned called outside Init state.");
    return false;
  }

  const auto& info = message_handler.info;
  auto loaned_cb = message_handler.callback;  // copy std::function (cheap)

  // Wrap LoanedMessageHandlerCallback into PodMessageCallback. POD path
  // has no ctx_ptr (publisher used kPodOnly mode) and the release
  // functor is a no-op because the iceoryx Listener thread released the
  // chunk synchronously after our PodMessageCallback returns.
  PodMessageCallback pod_cb =
      [loaned_cb = std::move(loaned_cb)](const void* payload, size_t size) {
        loaned_cb(/*ctx_ptr=*/nullptr, payload, size, /*release=*/[] {});
      };

  return RegisterPodSubscriber(info.topic_name, info.msg_type, std::move(pod_cb));
}

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
      auto url = MakeStdTransportUrl(info.topic_name, info.msg_type);
      auto* pub = iceoryx_bus_.GetPublisher(url);
      if (pub == nullptr) {
        MW_WARN("LoanSample[ctx-meta]: standard topic not registered. "
                "Call RegisterSendType() for topic={} msg_type={} first.",
                info.topic_name, info.msg_type);
        return false;
      }

      // Resolve serialization type the same way Send() does: prefer ctx, then
      // type-support default.
      std::string_view serialization_type = ctx->GetSerializationType();
      if (serialization_type.empty()) {
        serialization_type = info.msg_type_support_ref.DefaultSerializationType();
      }

      // Snapshot ctx-meta KVs. We carry *all* KVs (including the
      // serialization-type key) to stay byte-identical with Send()'s layout.
      auto kv_map = ctx->GetMetaKeyVals();

      size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv_map);
      if (header_size == 0) return false;  // overflow detected

      size_t total_size = header_size + requested_size;
      void* chunk = pub->LoanRaw(total_size);
      if (chunk == nullptr) return false;

      size_t written = EncodeCtxMetaHeader(
          chunk, total_size, static_cast<uint32_t>(total_size),
          serialization_type, kv_map);
      if (written != header_size) {
        // Should not happen; defensive release.
        pub->ReleaseRaw(chunk);
        return false;
      }

      auto* token = new CtxMetaLoanToken{pub, chunk, total_size};
      out_handle.user_payload_ptr =
          static_cast<void*>(static_cast<char*>(chunk) + header_size);
      out_handle.user_payload_size = requested_size;
      out_handle.mode = runtime::core::transport::LoanMode::kCtxMetaThenPayload;
      out_handle.driver_token = static_cast<void*>(token);
      out_handle.source_driver = this;  // Phase 7 §7.3 §K-3.3
      return true;
    } catch (const std::exception& e) {
      MW_ERROR("LoanSample[ctx-meta] exception: {}", e.what());
      return false;
    }
  }

  // kPodOnly path (ctx ignored).
  try {
    auto key = MakePodKey(info.topic_name, info.msg_type);
    auto it = pod_pub_map_.find(key);
    if (it == pod_pub_map_.end()) {
      MW_WARN("LoanSample: POD topic not registered. Call EnsurePodTopic first. "
              "topic={} msg_type={}", info.topic_name, info.msg_type);
      return false;
    }
    void* chunk = it->second->LoanRaw(requested_size);
    if (chunk == nullptr) return false;

    out_handle.user_payload_ptr = chunk;
    out_handle.user_payload_size = requested_size;
    out_handle.mode = runtime::core::transport::LoanMode::kPodOnly;
    out_handle.driver_token = static_cast<void*>(it->second);
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
// hint is available. Sized to comfortably hold a CompressedImage with a
// 1 KB data vector after CDR overhead, so the 1 KB benchmark row never
// hits the grow path. Larger payloads grow geometrically.
namespace {
constexpr size_t kInitialPayloadHint = 4 * 1024;
constexpr size_t kMaxLoanRetries = 16;  // 4 KB * 2^16 ~= 256 MB cap
}  // namespace

bool IceoryxTransportDriver::LoanSerializedSample(
    const runtime::core::transport::TransportInfo& info,
    const ibmw::transport::Context& ctx,
    const void* msg_ptr,
    runtime::core::transport::LoanedSampleHandle& out_handle) noexcept {
  try {
    if (msg_ptr == nullptr) return false;
    if (!info.msg_type_support_ref) {
      MW_WARN("LoanSerializedSample: msg_type_support_ref is empty -- "
              "caller must populate it (see IoxCtxMetaTypedPublisher).");
      return false;
    }

    auto url = MakeStdTransportUrl(info.topic_name, info.msg_type);
    auto* pub = iceoryx_bus_.GetPublisher(url);
    if (pub == nullptr) {
      MW_WARN("LoanSerializedSample: standard topic not registered. "
              "Call RegisterSendType() for topic={} msg_type={} first.",
              info.topic_name, info.msg_type);
      return false;
    }

    std::string_view serialization_type = ctx.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = info.msg_type_support_ref.DefaultSerializationType();
    }

    auto kv_map = ctx.GetMetaKeyVals();
    size_t header_size = ComputeCtxMetaHeaderSize(serialization_type, kv_map);
    if (header_size == 0) return false;

    // Start with header_size + a generous payload hint. On serialize-OOM we
    // double the chunk size and retry. Capped to avoid runaway growth.
    size_t chunk_size = header_size + kInitialPayloadHint;
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
        size_t total_size = header_size + actual_payload_size;
        size_t written = EncodeCtxMetaHeader(
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
      chunk_size *= 2;
    }

    if (!ok || chunk == nullptr) {
      MW_ERROR("LoanSerializedSample: gave up after {} growth retries "
              "(final chunk_size={})", kMaxLoanRetries, chunk_size);
      return false;
    }

    auto* token = new CtxMetaLoanToken{pub, chunk, header_size + actual_payload_size};
    out_handle.user_payload_ptr =
        static_cast<void*>(static_cast<char*>(chunk) + header_size);
    out_handle.user_payload_size = actual_payload_size;
    out_handle.mode = runtime::core::transport::LoanMode::kCtxMetaThenPayload;
    out_handle.driver_token = static_cast<void*>(token);
    out_handle.source_driver = this;  // Phase 7 §7.3 §K-3.3
    return true;
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
  if (handle.mode == runtime::core::transport::LoanMode::kCtxMetaThenPayload) {
    auto* token = static_cast<CtxMetaLoanToken*>(handle.driver_token);
    token->pub->PublishRaw(token->chunk_ptr);
    delete token;
  } else {
    auto* pub = static_cast<IoxPublisher*>(handle.driver_token);
    pub->PublishRaw(handle.user_payload_ptr);
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
  if (handle.mode == runtime::core::transport::LoanMode::kCtxMetaThenPayload) {
    auto* token = static_cast<CtxMetaLoanToken*>(handle.driver_token);
    token->pub->ReleaseRaw(token->chunk_ptr);
    delete token;
  } else {
    auto* pub = static_cast<IoxPublisher*>(handle.driver_token);
    pub->ReleaseRaw(handle.user_payload_ptr);
  }
  handle.user_payload_ptr = nullptr;
  handle.driver_token = nullptr;
  handle.user_payload_size = 0;
  handle.source_driver = nullptr;
}

}  // namespace ibmw::extensions::iceoryx_extension
