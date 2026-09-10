// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "generated/play_smoke_subscriber.h"

#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "cpp/util/buffer.h"
#include "dds_types/util/dds_type_support.h"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "type_support/dds/dds_transport.h"
#include "play_smoke_message.h"
#include "ros2_types/util/ros2_type_support_factory.h"

using Header = ibmw::protocols::common::Header;
using JsonHeader = ibmw::examples::cpp::play_smoke_test::JsonHeader;
using RosTestMsg = example_ros2::msg::RosTestMsg;

IBMW_DDS_TYPE_NAME(Header, "ibmw::protocols::common::Header")

namespace ibmw::examples::cpp::play_smoke_test::generated {
namespace {

ReceivedMessage Result(
    ibmw::transport::ContextRef context,
    uint64_t sequence,
    std::string payload,
    std::string_view decoded_serialization) {
  const auto context_serialization = context.GetSerializationType();
  return ReceivedMessage{
      .sequence = sequence,
      .payload = std::move(payload),
      .serialization = context_serialization.empty()
                           ? std::string(decoded_serialization)
                           : std::string(context_serialization),
      .driver = std::string(
          context.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER))};
}

const ibmw_string_view_t kJsonFirstSerializations[] = {
    ibmw::util::ToIbmwStringView("json"),
    ibmw::util::ToIbmwStringView("cdr")};

bool IsSerialization(ibmw_string_view_t value, std::string_view expected) {
  return value.str != nullptr &&
         std::string_view(value.str, value.len) == expected;
}

const ibmw_type_support_base_t* JsonFirstHeaderTypeSupport() {
  static const std::string kTypeName =
      "dds:ibmw::protocols::common::Header";
  static const auto* kCdr =
      ibmw::dds::GetDdsMessageTypeSupport<Header>(
          "ibmw::protocols::common::Header");
  static const ibmw_type_support_base_t kSupport{
      .type_name = [](void*) {
        return ibmw::util::ToIbmwStringView(kTypeName);
      },
      .create = [](void*) -> void* { return new JsonHeader(); },
      .destroy = [](void*, void* message) {
        delete static_cast<JsonHeader*>(message);
      },
      .copy = [](void*, const void* from, void* to) {
        *static_cast<JsonHeader*>(to) =
            *static_cast<const JsonHeader*>(from);
      },
      .move = [](void*, void* from, void* to) {
        *static_cast<JsonHeader*>(to) =
            std::move(*static_cast<JsonHeader*>(from));
      },
      .serialize = [](void*, ibmw_string_view_t serialization,
                      const void* message,
                      const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffers) {
        if (!IsSerialization(serialization, "json") || message == nullptr ||
            allocator == nullptr || allocator->allocate == nullptr ||
            buffers == nullptr) {
          return false;
        }
        try {
          const auto text =
              nlohmann::json(*static_cast<const JsonHeader*>(message)).dump();
          auto output = allocator->allocate(
              allocator->ctx, buffers, text.size());
          if (output.data == nullptr || output.len < text.size() ||
              buffers->len == 0) {
            return false;
          }
          std::memcpy(output.data, text.data(), text.size());
          buffers->data[buffers->len - 1].len = text.size();
          return true;
        } catch (...) {
          return false;
        }
      },
      .deserialize = [](void*, ibmw_string_view_t serialization,
                        ibmw_buffer_array_view_t buffers, void* message) {
        if (!IsSerialization(serialization, "json") || message == nullptr ||
            buffers.data == nullptr || buffers.len == 0) {
          return false;
        }
        try {
          std::string text;
          for (size_t index = 0; index < buffers.len; ++index) {
            text.append(
                static_cast<const char*>(buffers.data[index].data),
                buffers.data[index].len);
          }
          *static_cast<JsonHeader*>(message) =
              nlohmann::json::parse(text).get<JsonHeader>();
          return true;
        } catch (...) {
          return false;
        }
      },
      .serialization_types_supported_num = [](void*) { return size_t{2}; },
      .serialization_types_supported_list = [](void*) {
        return kJsonFirstSerializations;
      },
      .custom_type_support_ptr = [](void*) {
        return kCdr->custom_type_support_ptr(kCdr->ctx);
      },
      .ctx = nullptr};
  return &kSupport;
}

}  // namespace

bool RegisterCdrSubscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback) {
  return ibmw::transport::OnMessage<Header>(
      subscriber,
      [callback = std::move(callback)](
          ibmw::transport::ContextRef context,
          const std::shared_ptr<const Header>& message) {
        callback(Result(
            context, message->sequence_num(), message->frame_id(), "cdr"));
      });
}

bool RegisterJsonSubscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback) {
  return subscriber.OnMessage(
      JsonFirstHeaderTypeSupport(),
      [callback = std::move(callback)](
          const ibmw_transport_context_base_t* context,
          const void* message,
          ibmw_function_base_t* release_base) {
        ibmw::transport::SubscriberReleaseCallback release(release_base);
        const auto* value = static_cast<const JsonHeader*>(message);
        callback(Result(
            ibmw::transport::ContextRef(context), value->sequence_num,
            value->frame_id, "json"));
        release();
      });
}

bool RegisterRos2Subscriber(
    ibmw::transport::ReceiverRef subscriber, MessageCallback callback) {
  return subscriber.OnMessage(
      ibmw::GetRos2MessageTypeSupport<RosTestMsg>(),
      [callback = std::move(callback)](
          const ibmw_transport_context_base_t* context,
          const void* message,
          ibmw_function_base_t* release_base) {
        ibmw::transport::SubscriberReleaseCallback release(release_base);
        const auto* value = static_cast<const RosTestMsg*>(message);
        callback(Result(
            ibmw::transport::ContextRef(context),
            static_cast<uint64_t>(value->num),
            "ros2-" + std::to_string(value->num), "ros2"));
        release();
      });
}

}  // namespace ibmw::examples::cpp::play_smoke_test::generated
