// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_ext/zenoh_transport_driver.h"
#include "zenoh_ext/zenoh_serialized_message.h"
#include "zenoh_ext/discovery/zenoh_discovery_protocol.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::zenoh_extension::ZenohTransportDriver::Options> {
  using Options = ibmw::extensions::zenoh_extension::ZenohTransportDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["shm_enabled"] = pub_topic_options.shm_enabled;
      node["publishers"].push_back(pub_topic_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& pub_topic_options_node : node["publishers"]) {
        auto pub_topic_options = Options::PubTopicOptions{
            .topic_name = pub_topic_options_node["topic"].as<std::string>(),
            .shm_enabled = pub_topic_options_node["shm_enabled"].as<bool>(),
        };

        rhs.pub_topics_options.emplace_back(std::move(pub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::zenoh_extension {

void ZenohTransportDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Zenoh transport driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;
}

void ZenohTransportDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");
  zenoh_bus_ptr_->Freeze();
  zenoh_bus_ptr_->StartDiscovery(limit_domain_);
}

void ZenohTransportDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;
  zenoh_bus_ptr_->StopDiscovery();
  std::vector<uint64_t> handles;
  {
    std::lock_guard lock(native_mutex_);
    for (const auto& [handle, subscriber] : native_subscribers_) handles.push_back(handle);
  }
  for (const auto handle : handles) DetachSerializedSubscriber({handle});
  // The handlers use this driver: none may run once it is destroyed.
  for (const auto& [pattern, tool] : message_handler_map_) zenoh_bus_ptr_->UnregisterSubscriber(pattern);
}

runtime::core::transport::SerializedTransportOperationsV2
ZenohTransportDriver::SerializedOperations() noexcept {
  using namespace runtime::core::transport;
  return {.context = this,
          .supports_topic = [](void*, const TopicDescriptor& topic) noexcept {
            return !topic.msg_type.empty();
          },
          .attach_subscriber = [](void* driver, const SerializedSubscriberOptionsV2& options,
                                  SerializedMessageCallback callback) noexcept {
            return static_cast<ZenohTransportDriver*>(driver)->AttachSerializedSubscriber(options, std::move(callback));
          },
          .detach_subscriber = [](void* driver, SerializedSubscriberHandle handle) noexcept {
            return static_cast<ZenohTransportDriver*>(driver)->DetachSerializedSubscriber(handle);
          }};
}

runtime::core::transport::SerializedSubscriberHandle
ZenohTransportDriver::AttachSerializedSubscriber(
    const runtime::core::transport::SerializedSubscriberOptionsV2& options,
    runtime::core::transport::SerializedMessageCallback callback) noexcept {
  try {
    std::lock_guard lock(native_mutex_);
    MW_ASSERT(state_ == State::kInit || state_ == State::kStart, "Zenoh driver is not running.");
    MW_ASSERT(callback && ibmw::discovery::zenoh_protocol::IsConcreteKey(options.wire_topic_name) &&
                  options.wire_type_name == options.topic.msg_type,
              "Invalid native Zenoh subscription address or type.");
    auto subscriber = std::make_unique<NativeSubscriber>();
    subscriber->callback = std::make_shared<runtime::core::transport::SerializedCallback>(std::move(callback));
    const bool attached = zenoh_bus_ptr_->CreateSubscriber(options.wire_topic_name,
        [state = subscriber->callback, topic = options.topic,
         expected_key = options.wire_topic_name](const z_loaned_sample_t* sample) {
          try {
            z_view_string_t key;
            z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
            if (std::string_view(z_string_data(z_loan(key)), z_string_len(z_loan(key))) != expected_key)
              return;
            const auto* wire = z_sample_payload(sample);
            auto bytes = std::make_shared<std::string>(z_bytes_len(wire), '\0');
            auto reader = z_bytes_get_reader(wire);
            MW_ASSERT(z_bytes_reader_read(&reader, reinterpret_cast<uint8_t*>(bytes->data()), bytes->size()) == bytes->size(),
                      "Cannot read Zenoh sample.");
            const auto decoded = DecodeZenohPayload(*bytes);
            auto descriptor = topic;
            descriptor.serialization_type = decoded.serialization;
            // Schemas are resolved by the consumer using the actual format.
            if (descriptor.serialization_type != topic.serialization_type) descriptor.schema = {};
            auto payload = std::shared_ptr<ibmw::util::BufferArrayView>(
                new ibmw::util::BufferArrayView(decoded.payload.data(), decoded.payload.size()),
                [bytes](auto* view) { delete view; });
            state->Invoke({.topic = std::move(descriptor),
                           .receive_timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count()),
                           .source_driver = "zenoh", .payload = std::move(payload)});
          } catch (const std::exception& error) {
            MW_ERROR("Native Zenoh receive failed: {}", error.what());
          }
        }, subscriber->subscription);
    MW_ASSERT(attached, "Cannot declare native Zenoh subscriber.");
    const uint64_t handle = next_native_handle_++;
    native_subscribers_.emplace(handle, std::move(subscriber));
    return {handle};
  } catch (const std::exception& error) {
    MW_ERROR("Cannot attach native Zenoh subscriber: {}", error.what());
    return {};
  }
}

bool ZenohTransportDriver::DetachSerializedSubscriber(
    runtime::core::transport::SerializedSubscriberHandle handle) noexcept {
  std::unique_ptr<NativeSubscriber> subscriber;
  {
    std::lock_guard lock(native_mutex_);
    const auto found = native_subscribers_.find(handle.value);
    if (found == native_subscribers_.end()) return false;
    subscriber = std::move(found->second);
    native_subscribers_.erase(found);
  }
  subscriber.reset();
  return true;
}

bool ZenohTransportDriver::RegisterSendType(
    const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = send_type_wrapper.info;

    bool shm_enabled = false;

    auto find_option_itr = std::find_if(
        options_.pub_topics_options.begin(), options_.pub_topics_options.end(),
        [topic_name = info.topic_name](const Options::PubTopicOptions& pub_option) {
          try {
            return std::regex_match(topic_name.begin(), topic_name.end(), std::regex(pub_option.topic_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       pub_option.topic_name, topic_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.pub_topics_options.end()) {
      shm_enabled = find_option_itr->shm_enabled;
    }

    namespace util = ibmw::common::util;
    std::string pattern = std::string("transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type) +
                          key_suffix_;

    if (!zenoh_bus_ptr_->RegisterPublisher(pattern, shm_enabled)) return false;
    zenoh_bus_ptr_->RecordDiscoveryEndpoint(ibmw::discovery::EndpointRole::kPublisher,
                                          info.topic_name, info.msg_type, pattern);

    shm_loan_sizes_.Set(pattern, shm_init_loan_size_);

    MW_INFO("Register publish type to zenoh transport, url: {}, shm_enabled: {}", pattern, shm_enabled);

    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool ZenohTransportDriver::OnMessage(
    const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept {
  try {
    MW_REQUIRE(state_.load() == State::kInit,
                            "Method can only be called when state is 'Init'.");

    const auto& info = message_handler.info;
    namespace util = ibmw::common::util;

    std::string pattern = std::string("transport/") +
                          util::UrlEncode(info.topic_name) + "/" +
                          util::UrlEncode(info.msg_type) +
                          key_suffix_;

    auto find_itr = message_handler_map_.find(pattern);
    if (find_itr != message_handler_map_.end()) {
      find_itr->second->AddMessageHandlerWrapper(&message_handler);
      return true;
    }

    auto msg_handler_tool_unique_ptr = std::make_unique<ibmw::runtime::core::transport::MessageHandlerTool>();
    msg_handler_tool_unique_ptr->AddMessageHandlerWrapper(&message_handler);

    auto* msg_handler_tool_ptr = msg_handler_tool_unique_ptr.get();

    auto handle =
        [this, topic_name = info.topic_name, msg_handler_tool_ptr](const z_loaned_sample_t* message) {
          try {
            auto ctx_ptr = std::make_shared<ibmw::transport::Context>(ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

            // todo(hj): avoid copy when use shm
            const z_loaned_bytes_t* payload = z_sample_payload(message);
            size_t serialized_size = z_bytes_len(payload);
            z_bytes_reader_t reader = z_bytes_get_reader(payload);
            std::vector<char> serialized_data(serialized_size);

            // read data from payload
            serialized_data.resize(
                z_bytes_reader_read(&reader, reinterpret_cast<uint8_t*>(serialized_data.data()), serialized_size));

            // The length prefix comes from the peer: validate it against the bytes actually received.
            auto frame = util::GetLengthPrefixedPayload(serialized_data.data(), serialized_data.size());
            util::ConstBufferOperator buf_oper(frame.data(), frame.size());

            // get serialization type
            std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
            ctx_ptr->SetSerializationType(serialization_type);

            // get context meta
            size_t ctx_num = buf_oper.GetUint8();
            for (size_t ii = 0; ii < ctx_num; ++ii) {
              auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
              auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
              ctx_ptr->SetMetaValue(key, val);
            }
            ctx_ptr->SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER, Name());

            // get msg
            auto remaining_buf = buf_oper.GetRemainingBuffer();

            msg_handler_tool_ptr->DoMessageCallback(
                ctx_ptr, serialization_type, static_cast<const void*>(remaining_buf.data()), remaining_buf.size());

          } catch (const std::exception& e) {
            MW_WARN("Handle Zenoh transport msg failed, exception info: {}", e.what());
          }
        };

    if (!zenoh_bus_ptr_->RegisterSubscriber(pattern, std::move(handle))) return false;
    message_handler_map_.emplace(pattern, std::move(msg_handler_tool_unique_ptr));
    zenoh_bus_ptr_->RecordDiscoveryEndpoint(ibmw::discovery::EndpointRole::kSubscriber,
                                          info.topic_name, info.msg_type, pattern);
    MW_INFO("Register subscribe type to zenoh transport, url: {}", pattern);
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

void ZenohTransportDriver::Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept {
  try {
    // checkout state
    MW_REQUIRE(state_.load() == State::kStart,
                            "Method can only be called when state is 'Start'.");

    namespace util = ibmw::common::util;
    const auto& info = msg_wrapper.info;

    std::string zenoh_pub_topic = std::string("transport/") +
                                  util::UrlEncode(info.topic_name) + "/" +
                                  util::UrlEncode(info.msg_type) +
                                  key_suffix_;

    // find publisher with zenoh_pub_topic
    const auto& zenoh_pub_catalog_ptr = zenoh_bus_ptr_->GetPublisherRegisterMap();
    auto z_pub_iter = zenoh_pub_catalog_ptr.find(zenoh_pub_topic);
    if (z_pub_iter == zenoh_pub_catalog_ptr.end()) {
      MW_ERROR("Url: {} is not registered!", zenoh_pub_topic);
      return;
    }

    // get serialization type
    auto publish_type_support_ref = info.msg_type_support_ref;

    auto serialization_type = msg_wrapper.ctx_ref.GetSerializationType();
    if (serialization_type.empty()) {
      serialization_type = publish_type_support_ref.DefaultSerializationType();
    }

    // get meta data
    auto [meta_key_vals_array, meta_key_vals_array_len] = msg_wrapper.ctx_ref.GetMetaKeyValsArray();
    MW_REQUIRE(meta_key_vals_array_len / 2 <= 255,
                            "Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    auto z_pub_ctx_ptr = z_pub_iter->second;
    // shm_enabled
    if (z_pub_ctx_ptr->shm_enabled) {
      unsigned char* z_pub_loaned_shm_ptr = nullptr;
      std::shared_ptr<ibmw::util::BufferArrayView> buffer_array_cache_ptr = nullptr;

      bool is_shm_loan_size_enough = true;
      bool is_shm_pool_size_enough = true;

      uint64_t msg_size = 0;
      ShmLoan loan;
      const uint64_t pool_size = zenoh_bus_ptr_->ShmPoolSize();

      do {
        // loan a new size shm (a loan that was too small is dropped)
        const uint64_t loan_size = shm_loan_sizes_.Get(zenoh_pub_topic, shm_init_loan_size_);
        z_pub_loaned_shm_ptr = loan.Alloc(z_loan(zenoh_bus_ptr_->shm_provider_), loan_size, zenoh_bus_ptr_->alignment_);

        // if shm pool is not enough, use net buffer instead
        if (z_pub_loaned_shm_ptr == nullptr) {
          is_shm_pool_size_enough = false;
          break;
        }

        // write info pkg on loaned shm : the first FIXED_LEN bytes needs to write the length of pkg
        util::BufferOperator buf_oper(reinterpret_cast<char*>(z_pub_loaned_shm_ptr) + 4, loan_size - 4);

        // write serialization type on loaned shm
        buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

        // write context meta on loaned shm
        buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
        for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
          buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
        }
        auto type_and_ctx_len = 1 + serialization_type.size() + context_meta_kv_size;

        // write msg on loaned shm: should start at the (FIXED_LEN + type_and_ctx_len)-th byte
        ibmw::util::ZenohBufferArrayAllocator z_allocator(buf_oper.GetRemainingSize(), z_pub_loaned_shm_ptr + type_and_ctx_len + 4);
        if (buffer_array_cache_ptr == nullptr) {
          try {
            auto result = SerializeMsgSupportedZenoh(msg_wrapper, serialization_type, ibmw::util::BufferArrayAllocatorRef(z_allocator.NativeHandle()));
            msg_size = result.second;
            buffer_array_cache_ptr = result.first;
            if (buffer_array_cache_ptr == nullptr) {
              // in this case means no cache is set, then do nomal serialization(if size is small will throw exception)
              is_shm_loan_size_enough = true;
            } else {
              if (msg_size > buf_oper.GetRemainingSize()) {
                // in this case means the msg has serialization cache but the size is too large, then expand suitable size
                is_shm_loan_size_enough = false;
                if (!shm_loan_sizes_.Need(zenoh_pub_topic, msg_size + type_and_ctx_len + 4, pool_size)) {
                  is_shm_pool_size_enough = false;
                  break;
                }
              } else {
                // in this case means the msg has serialization cache and the size is suitable, then use cachema
                is_shm_loan_size_enough = true;
              }
            }

          } catch (const std::exception& e) {
            if (!z_allocator.IsShmEnough()) {
              // the shm is not enough, need to expand a double size (within the pool)
              is_shm_loan_size_enough = false;
              if (!shm_loan_sizes_.Grow(zenoh_pub_topic, loan_size, pool_size)) {
                is_shm_pool_size_enough = false;
                break;
              }
            } else {
              MW_ERROR(
                  "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}, exception: {}",
                  serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type, e.what());
              return;
            }
          }
        } else {
          // the cache was loaned its size (Need above): it fits now
          is_shm_loan_size_enough = true;
        }

      } while (!is_shm_loan_size_enough);

      if (is_shm_pool_size_enough) {
        // if has cache, the copy it to shm to replace the serialization
        if (buffer_array_cache_ptr != nullptr) {
          CopyBuffers(*buffer_array_cache_ptr, z_pub_loaned_shm_ptr + 4 + context_meta_kv_size + serialization_type.size() + 1);

          buffer_array_cache_ptr = nullptr;
        }
        // write info pkg length on loaned shm
        uint32_t data_size = 1 + serialization_type.size() + context_meta_kv_size + msg_size;
        util::SetBufFromUint32(reinterpret_cast<char*>(z_pub_loaned_shm_ptr), data_size);

        z_owned_bytes_t z_payload;
        loan.MoveTo(&z_payload);
        z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);

        // collect shared memory whose reference counting is zero (defragment only when short)
        z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));

        MW_TRACE("Zenoh publish to '{}'", zenoh_pub_topic);

        return;
      }
      // the loan that did not fit (up to the whole pool) is collected now, not at the next shortage
      loan.Reset();
      z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));
      MW_WARN_INTERVAL(5000, "Zenoh Extension shm pool is not enough, use net buffer instead.");
    }

    // shm disabled
    // serialize msg
    auto buffer_array_view_ptr = ibmw::runtime::core::transport::SerializeMsgWithCache(msg_wrapper, serialization_type);
    MW_REQUIRE(
        buffer_array_view_ptr,
        "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, topic_name: {}, msg_type: {}",
        serialization_type, info.pkg_path, info.module_name, info.topic_name, info.msg_type);

    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t msg_size = buffer_array_view_ptr->BufferSize();

    int32_t data_size = 1 + serialization_type.size() + context_meta_kv_size + msg_size;
    int32_t pkg_size = data_size + 4;

    // create buffer for serialization
    std::vector<char> serialized_data(pkg_size);
    util::BufferOperator buf_oper(serialized_data.data(), pkg_size);

    // full data_size
    buf_oper.SetUint32(data_size);

    // full serialization_type
    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

    // full context meta
    buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
    }

    // full msg
    for (size_t ii = 0; ii < buffer_array_len; ++ii) {
      buf_oper.SetBuffer(
          static_cast<const char*>(buffer_array_data[ii].data),
          buffer_array_data[ii].len);
    }

    // publish
    z_owned_bytes_t z_payload;
    z_bytes_from_buf(&z_payload, reinterpret_cast<uint8_t*>(serialized_data.data()), pkg_size, nullptr, nullptr);
    z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);

    MW_TRACE("Zenoh publish to '{}'", zenoh_pub_topic);

    return;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

}  // namespace ibmw::extensions::zenoh_extension
