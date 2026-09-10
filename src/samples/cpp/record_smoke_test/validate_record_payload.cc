// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "record_playback_ext/storage/bag_reader_factory.h"
#include "record_smoke_message.h"
#include "cpp/util/type_support.h"
#include "engine/type_support_pkg_loader.h"

namespace {

using ibmw::extensions::record_playback_extension::BagMessage;
using ibmw::extensions::record_playback_extension::CreateBagReader;
using ibmw::extensions::record_playback_extension::ReaderOpenOptions;
using ibmw::runtime::core::util::TypeSupportPkgLoader;
using Header = ibmw::protocols::common::Header;
using JsonSmokeMessage =
    ibmw::examples::cpp::record_smoke_test::JsonSmokeMessage;
using RosTestMsg = example_ros2::msg::RosTestMsg;

const ibmw_type_support_base_t* FindTypeSupport(
    TypeSupportPkgLoader& loader,
    std::string_view type_name) {
  for (const auto* candidate : loader.GetTypeSupportArray()) {
    if (candidate == nullptr || candidate->type_name == nullptr) continue;
    ibmw::util::TypeSupportRef ref(candidate);
    if (ref.TypeName() == type_name) return candidate;
  }
  return nullptr;
}

bool ValidateMessage(
    const std::string& type_name,
    const ibmw::util::TypeSupportRef& support,
    const BagMessage& message,
    uint64_t* decoded_sequence) {
  if (message.payload == nullptr || message.payload->NativeHandle() == nullptr) {
    std::cerr << "payload is null\n";
    return false;
  }
  if (message.payload->BufferSize() == 0) {
    std::cerr << "payload is empty\n";
    return false;
  }
  void* raw = support.Create();
  if (raw == nullptr) return false;
  const bool decoded = support.Deserialize(
      message.channel.topic.serialization_type,
      *message.payload->NativeHandle(), raw);
  if (!decoded) {
    support.Destroy(raw);
    std::cerr << "deserialize failed for " << type_name << '\n';
    return false;
  }

  bool valid = true;
  if (type_name == "dds:ibmw::protocols::common::Header") {
    const auto& value = *static_cast<const Header*>(raw);
    *decoded_sequence = value.sequence_num();
    valid = value.frame_id() == "record_smoke";
  } else if (type_name == "json:ibmw.record_smoke.JsonSmokeMessage") {
    const auto& value = *static_cast<const JsonSmokeMessage*>(raw);
    *decoded_sequence = value.sequence;
    valid = value.payload == "record-smoke-" + std::to_string(value.sequence);
  } else if (type_name == "ros2:example_ros2/msg/RosTestMsg") {
    const auto& value = *static_cast<const RosTestMsg*>(raw);
    *decoded_sequence = static_cast<uint64_t>(value.num);
    valid =
            value.data == std::vector<uint8_t>({1, 2, 3, 4}) &&
            value.test_msg2.num == value.num &&
            value.test_msg2.num2 == 1.0 && value.test_msg2.data == 1;
  }
  support.Destroy(raw);
  if (!valid) {
    std::cerr << "decoded payload mismatch (type " << type_name << ")\n";
  }
  return valid;
}

int Run(const std::filesystem::path& bag_path,
        const std::string& expected_topic,
        const std::string& expected_serialization,
        const std::filesystem::path& type_support_package) {
  const auto serialization =
      expected_serialization == "auto" ? std::string("cdr")
                                       : expected_serialization;
  auto reader = CreateBagReader(bag_path);
  const auto& info = reader->Info();
  const auto channel = std::find_if(
      info.channels.begin(), info.channels.end(), [&](const auto& item) {
        return item.topic.topic == expected_topic &&
               item.topic.serialization_type == serialization;
      });
  if (channel == info.channels.end()) {
    throw std::runtime_error("expected topic/serialization is absent from bag");
  }

  TypeSupportPkgLoader loader;
  loader.LoadTypeSupportPkg(type_support_package.string());
  const auto* native = FindTypeSupport(loader, channel->topic.msg_type);
  if (native == nullptr) {
    throw std::runtime_error("type support package does not export " +
                             channel->topic.msg_type);
  }
  ibmw::util::TypeSupportRef support(native);

  uint64_t count = 0;
  uint32_t previous_bag_sequence = 0;
  uint64_t previous_log_time = 0;
  uint64_t previous_decoded_sequence = 0;
  bool have_decoded_sequence = false;
  bool first = true;
  bool valid = true;
  reader->ReadMessages({}, [&](BagMessage&& message) {
    if (message.channel.topic.topic != expected_topic) {
      valid = false;
      return false;
    }
    if (!first && (message.sequence <= previous_bag_sequence ||
                   message.sequence != previous_bag_sequence + 1 ||
                   message.log_time < previous_log_time)) {
      valid = false;
      return false;
    }
    first = false;
    previous_bag_sequence = message.sequence;
    previous_log_time = message.log_time;
    uint64_t decoded_sequence = 0;
    if (!ValidateMessage(channel->topic.msg_type, support, message,
                         &decoded_sequence) ||
        (have_decoded_sequence && decoded_sequence <= previous_decoded_sequence)) {
      valid = false;
      return false;
    }
    previous_decoded_sequence = decoded_sequence;
    have_decoded_sequence = true;
    ++count;
    return true;
  });
  if (!valid || count != channel->message_count) {
    throw std::runtime_error(
        "payload validation failed: decoded " + std::to_string(count) +
        ", expected " + std::to_string(channel->message_count));
  }
  std::cout << "PASS: payload " << expected_topic << " (" << count
            << " messages, " << channel->topic.msg_type << ")\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: validate_record_payload BAG TOPIC SERIALIZATION TYPE_SUPPORT_PACKAGE\n";
    return 2;
  }
  try {
    return Run(argv[1], argv[2], argv[3], argv[4]);
  } catch (const std::exception& error) {
    std::cerr << "FAIL: record payload validation: " << error.what() << '\n';
    return 1;
  }
}
